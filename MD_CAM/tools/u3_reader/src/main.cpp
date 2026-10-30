/**
 * @file main.cpp
 * @brief Read-only SPI NOR dumper for U3 (EON EN25F40, JEDEC 1C 31 13) on GW MD CAM2.
 *
 * Serial commands (one byte):
 *   'I' -> "ID xx xx xx\n"
 *   'R' -> "DUMP <len>\n", <len> raw bytes, "CRC <crc32 hex>\n"
 * Only read commands (0x9F, 0x03) are ever sent to the flash.
 */

#include <Arduino.h>
#include <SPI.h>

#if CONFIG_IDF_TARGET_ESP32S2
static const int PIN_SCK  = 7;
static const int PIN_MISO = 9;
static const int PIN_MOSI = 11;
static const int PIN_CS   = 12;
#elif CONFIG_IDF_TARGET_ESP32C3
static const int PIN_SCK  = 4; // GPIO 2/8/9 är strapping-pinnar på C3 och undviks
static const int PIN_MISO = 5;
static const int PIN_MOSI = 6;
static const int PIN_CS   = 7;
#else
static const int PIN_SCK  = 18;
static const int PIN_MISO = 19;
static const int PIN_MOSI = 23;
static const int PIN_CS   = 5;
#endif

static const uint32_t FLASH_SIZE = 512UL * 1024UL;
static const uint32_t SPI_HZ     = 250000; // lågt för långa klämtrådar
static const size_t   CHUNK      = 256;

static uint8_t s_buf[CHUNK];

static void flashSelect(bool on) {
    digitalWrite(PIN_CS, on ? LOW : HIGH);
    delayMicroseconds(2);
}

static void readJedec(uint8_t id[3]) {
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    flashSelect(true);
    SPI.transfer(0x9F);
    for (int i = 0; i < 3; i++) id[i] = SPI.transfer(0x00);
    flashSelect(false);
    SPI.endTransaction();
}

static uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0 - (crc & 1)));
    }
    return ~crc;
}

static void dumpFlash(void) {
    uint32_t crc = 0;

    Serial.printf("DUMP %lu\n", (unsigned long)FLASH_SIZE);
    SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
    flashSelect(true);
    SPI.transfer(0x03);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    SPI.transfer(0x00);
    for (uint32_t off = 0; off < FLASH_SIZE; off += CHUNK) {
        for (size_t i = 0; i < CHUNK; i++) s_buf[i] = SPI.transfer(0x00);
        crc = crc32Update(crc, s_buf, CHUNK);
        Serial.write(s_buf, CHUNK);
    }
    flashSelect(false);
    SPI.endTransaction();
    Serial.flush();
    Serial.printf("\nCRC %08lx\n", (unsigned long)crc);
}

// 'D': visar om något på kortet håller fast linjerna. Varje rad är nivån på
// CS/MISO/MOSI/SCK med intern pull-up, med intern pull-down, och när ESP:n själv
// driver linjen låg resp. hög (läst tillbaka från padden).
static void diagnose(void) {
    const int pins[4] = {PIN_CS, PIN_MISO, PIN_MOSI, PIN_SCK};
    const char *names[4] = {"CS", "MISO", "MOSI", "SCK"};

    SPI.end();
    for (int i = 0; i < 4; i++) {
        gpio_num_t g = (gpio_num_t)pins[i];
        pinMode(pins[i], INPUT_PULLUP);
        delay(2);
        int pu = digitalRead(pins[i]);
        pinMode(pins[i], INPUT_PULLDOWN);
        delay(2);
        int pd = digitalRead(pins[i]);
        gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(g, 0);
        delay(2);
        int d0 = gpio_get_level(g);
        gpio_set_level(g, 1);
        delay(2);
        int d1 = gpio_get_level(g);
        pinMode(pins[i], INPUT);
        Serial.printf("DIAG %-4s pullup=%d pulldown=%d drive0=%d drive1=%d\n", names[i], pu, pd, d0, d1);
    }
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
    Serial.println("DIAG END");
}

void setup() {
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    Serial.begin(921600);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
    // Svagaste drivstyrkan ger mjukare flanker och mindre ringning på lösa trådar
    gpio_set_drive_capability((gpio_num_t)PIN_SCK, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)PIN_MOSI, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)PIN_CS, GPIO_DRIVE_CAP_0);
}

void loop() {
    if (!Serial.available()) return;
    int c = Serial.read();
    if (c == 'I') {
        uint8_t id[3];
        readJedec(id);
        Serial.printf("ID %02x %02x %02x\n", id[0], id[1], id[2]);
    } else if (c == 'R') {
        dumpFlash();
    } else if (c == 'D') {
        diagnose();
    }
}
