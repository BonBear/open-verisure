#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#ifndef PIN_TCK
#define PIN_TCK 0
#endif

#ifndef PIN_RST
#define PIN_RST 4
#endif

#ifndef PIN_BSL_RX
#define PIN_BSL_RX 2
#endif

#ifndef PIN_BSL_TX
#define PIN_BSL_TX 3
#endif

static char s_line[640];
static size_t s_lineLen = 0U;
static bool s_lineOverflow = false;

static uint8_t s_tx[260];
static uint8_t s_rx[260];
static char s_hexOut[524];

static bool s_entered = false;

static void setAllPinsHiZ() {
    pinMode(PIN_TCK, INPUT);
    pinMode(PIN_RST, INPUT);
    pinMode(PIN_BSL_RX, INPUT);
    pinMode(PIN_BSL_TX, INPUT);
}

static void drainBslRx() {
    while (Serial1.available()) {
        (void)Serial1.read();
    }
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void encodeHex(const uint8_t *buf, size_t len, char *out, size_t outSize) {
    static const char kHex[] = "0123456789ABCDEF";
    size_t pos = 0U;
    for (size_t i = 0U; i < len && (pos + 2U) < outSize; i++) {
        out[pos++] = kHex[(buf[i] >> 4) & 0x0FU];
        out[pos++] = kHex[buf[i] & 0x0FU];
    }
    if (outSize > 0U) {
        out[pos] = '\0';
    }
}

static bool parseUint(const char *s, uint32_t minVal, uint32_t maxVal, uint32_t *out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    uint32_t val = 0U;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint32_t digit = (uint32_t)(*p - '0');
        if (val > (maxVal - digit) / 10U) {
            return false;
        }
        val = val * 10U + digit;
    }
    if (val < minVal || val > maxVal) {
        return false;
    }
    *out = val;
    return true;
}

static char *nextToken(char **cursor) {
    char *p = *cursor;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return nullptr;
    }
    char *tok = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p != '\0') {
        *p = '\0';
        p++;
    }
    *cursor = p;
    return tok;
}

static void handleEnter() {
    if (s_entered) {
        Serial1.end();
        setAllPinsHiZ();
        delay(5);
        s_entered = false;
    }

    if (digitalRead(PIN_RST) != HIGH) {
        Serial.println("ERR RST_LOW");
        return;
    }

    // TI SLAU319 §1.3.1.2 ROM BSL entry sequence (devices without TEST pin)
    digitalWrite(PIN_TCK, HIGH);
    pinMode(PIN_TCK, OUTPUT);
    delay(1);

    digitalWrite(PIN_RST, LOW);
    pinMode(PIN_RST, OUTPUT);
    delay(5);

    // Initialize UART while RST is LOW so P2.2 (BSL RXD) idles HIGH before RST rises
    Serial1.setRxBufferSize(1024);
    Serial1.begin(9600, SERIAL_8E1, PIN_BSL_RX, PIN_BSL_TX);
    delay(1);

    // Two falling edges on TCK while RST == LOW; TCK stays LOW
    digitalWrite(PIN_TCK, LOW);
    delayMicroseconds(500);
    digitalWrite(PIN_TCK, HIGH);
    delayMicroseconds(500);
    digitalWrite(PIN_TCK, LOW);
    delayMicroseconds(500);

    // Release RST HIGH while TCK == LOW, then return TCK HIGH
    digitalWrite(PIN_RST, HIGH);
    delay(2);
    digitalWrite(PIN_TCK, HIGH);

    delay(10);
    drainBslRx();
    s_entered = true;
    Serial.println("OK");
}

static void handleSync() {
    if (!s_entered) {
        Serial.println("ERR NOT_ENTERED");
        return;
    }

    drainBslRx();
    Serial1.write((uint8_t)0x80U);
    Serial1.flush();

    uint32_t startMs = millis();
    while (!Serial1.available()) {
        if ((uint32_t)(millis() - startMs) >= 200U) {
            Serial.println("ERR TIMEOUT");
            return;
        }
        delay(1);
    }

    int rx = Serial1.read();
    if (rx < 0) {
        Serial.println("ERR TIMEOUT");
        return;
    }
    uint8_t b = (uint8_t)rx;
    if (b == 0x90U) {
        Serial.println("OK 90");
    } else {
        Serial.printf("ERR NAK %02X\r\n", (unsigned int)b);
    }
}

static void handleXfer(char *cursor) {
    if (!s_entered) {
        Serial.println("ERR NOT_ENTERED");
        return;
    }

    const char *hexTx = nextToken(&cursor);
    const char *expectTok = nextToken(&cursor);
    const char *timeoutTok = nextToken(&cursor);
    const char *extraTok = nextToken(&cursor);

    if (hexTx == nullptr || expectTok == nullptr || extraTok != nullptr) {
        Serial.println("ERR BAD_ARGS");
        return;
    }

    size_t hexLen = strlen(hexTx);
    if (hexLen < 2U || hexLen > 510U || (hexLen & 1U) != 0U) {
        Serial.println("ERR BAD_ARGS");
        return;
    }

    size_t txLen = hexLen / 2U;
    for (size_t i = 0U; i < txLen; i++) {
        int hi = hexVal(hexTx[i * 2U]);
        int lo = hexVal(hexTx[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            Serial.println("ERR BAD_ARGS");
            return;
        }
        s_tx[i] = (uint8_t)((hi << 4) | lo);
    }

    uint32_t expectRxLen = 0U;
    if (!parseUint(expectTok, 0U, 255U, &expectRxLen)) {
        Serial.println("ERR BAD_ARGS");
        return;
    }

    uint32_t timeoutMs = 500U;
    if (timeoutTok != nullptr && !parseUint(timeoutTok, 1U, 10000U, &timeoutMs)) {
        Serial.println("ERR BAD_ARGS");
        return;
    }

    drainBslRx();
    Serial1.write(s_tx, txLen);
    Serial1.flush();

    if (expectRxLen == 0U) {
        Serial.println("OK");
        return;
    }

    uint32_t startMs = millis();
    while (!Serial1.available()) {
        if ((uint32_t)(millis() - startMs) >= timeoutMs) {
            Serial.println("ERR TIMEOUT");
            return;
        }
        delay(1);
    }

    size_t rxLen = 0U;
    uint32_t lastByteMs = millis();
    while (rxLen < expectRxLen) {
        if (Serial1.available()) {
            int b = Serial1.read();
            if (b >= 0) {
                s_rx[rxLen++] = (uint8_t)b;
                lastByteMs = millis();
                continue;
            }
        }
        if ((uint32_t)(millis() - lastByteMs) >= 50U) {
            break;
        }
        delay(1);
    }

    if (rxLen == 0U) {
        Serial.println("ERR TIMEOUT");
        return;
    }

    encodeHex(s_rx, rxLen, s_hexOut, sizeof(s_hexOut));
    if (rxLen < expectRxLen) {
        Serial.printf("ERR SHORT %s\r\n", s_hexOut);
    } else {
        Serial.printf("OK %s\r\n", s_hexOut);
    }
}

static void handleRelease() {
    bool wasEntered = s_entered;
    if (s_entered) {
        Serial1.end();
        s_entered = false;
    }

    if (wasEntered || digitalRead(PIN_RST) == HIGH) {
        digitalWrite(PIN_RST, LOW);
        pinMode(PIN_RST, OUTPUT);
        delay(5);
    }

    setAllPinsHiZ();
    Serial.println("OK");
}

static void dispatchLine(char *line) {
    char *cursor = line;
    const char *cmd = nextToken(&cursor);
    if (cmd == nullptr) {
        return;
    }

    if (strcmp(cmd, "XFER") == 0) {
        handleXfer(cursor);
        return;
    }

    bool hasExtra = (nextToken(&cursor) != nullptr);
    if (strcmp(cmd, "PING") == 0) {
        if (hasExtra) {
            Serial.println("ERR BAD_ARGS");
        } else {
            Serial.println("OK BSL_BRIDGE");
        }
    } else if (strcmp(cmd, "ENTER") == 0) {
        if (hasExtra) {
            Serial.println("ERR BAD_ARGS");
        } else {
            handleEnter();
        }
    } else if (strcmp(cmd, "SYNC") == 0) {
        if (hasExtra) {
            Serial.println("ERR BAD_ARGS");
        } else {
            handleSync();
        }
    } else if (strcmp(cmd, "RELEASE") == 0) {
        if (hasExtra) {
            Serial.println("ERR BAD_ARGS");
        } else {
            handleRelease();
        }
    } else {
        Serial.println("ERR UNKNOWN_CMD");
    }
}

void setup() {
    setAllPinsHiZ();
    Serial.setRxBufferSize(4096);
    Serial.setTxTimeoutMs(20);
    Serial.begin(115200);
}

void loop() {
    while (Serial.available()) {
        int ch = Serial.read();
        if (ch < 0) {
            break;
        }
        char c = (char)ch;
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (s_lineOverflow) {
                s_lineOverflow = false;
                s_lineLen = 0U;
                Serial.println("ERR LINE_TOO_LONG");
            } else if (s_lineLen > 0U) {
                s_line[s_lineLen] = '\0';
                s_lineLen = 0U;
                dispatchLine(s_line);
            }
            continue;
        }
        if (s_lineOverflow) {
            continue;
        }
        if (s_lineLen >= (sizeof(s_line) - 1U)) {
            s_lineOverflow = true;
        } else {
            s_line[s_lineLen++] = c;
        }
    }
    delay(1);
}

