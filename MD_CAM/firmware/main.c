/**
 * @file main.c
 * @brief Verisure Motion Detector Camera (GW MD CAM2 / SECUR-PCAM-BLACKFIN) - CC1110F32 Firmware
 *
 * 868.950 MHz AES-128 telemetry with PM2 Deep Sleep:
 *  - PIR Motion (P1_7, active LOW) wakes the CPU instantly via Port 1 interrupt.
 *  - Sleep Timer wakes the CPU every 0.5 s to sample the rear tamper switch SW3
 *    (dual pull-up/pull-down scan of P0_7/P0_6/P0_0/P1_6/P2_0 compared with the
 *    boot baseline) and to count down the periodic heartbeat.
 *  - 27 MHz XOSC + radio are only powered while transmitting.
 *
 * Tamper polarity (wall mount): batteries are ALWAYS inserted while the unit is off the
 * wall, so the boot state = SW3 released = TAMPER. SW3 pressed against the wall = CLEAR.
 *
 * Pin map: see Hardware_Pinout_och_Matprotokoll.md (J6: DD=P2_1, DC=P2_2, RESET_N).
 */

#include <cc1110.h>
#include <stdint.h>
#include <stdbool.h>

#if __has_include("config.h")
#include "config.h"
#elif __has_include("config.h.example")
#include "config.h.example"
#else
#error "No config.h or config.h.example found! Please generate config.h with derive_key.py."
#endif
#include "camera_image.h"

/** @brief Fixed packet length: 8B Header + 16B AES Ciphertext. */
#define PACKET_LEN        24

/** @brief Sleep Timer tick: WOR_RES=2 (1024 x 32 kHz periods = 31.25 ms) x 16 = 0.5 s. */
#define ST_WOR_RES        0x02
#define ST_WOREVT         16

/** @brief Heartbeat interval in 0.5 s ticks (7200 = 1 hour). */
#define HEARTBEAT_TICKS   7200

typedef struct {
    uint8_t src_h;
    uint8_t src_l;
    uint8_t dst_h;
    uint8_t dst_l;
    uint8_t len_h;
    uint8_t len_l;
    uint8_t cfg0;
    uint8_t cfg1;
} dma_desc_t;

/* Global variables in XRAM (__xdata) - zero dynamic allocation */
static __xdata dma_desc_t dma_desc;
static __xdata uint8_t tx_buf[PACKET_LEN];

volatile __xdata uint8_t g_tx_count = 0;
volatile __xdata uint8_t g_tx_fail = 0;

volatile __xdata uint8_t g_uart_rx_buf[64];
volatile uint8_t g_uart_rx_count = 0;
volatile __xdata uint8_t g_diag_p16 = 0;
volatile __xdata uint8_t g_diag_p16_trans = 0;

/** @brief Set by Sleep Timer ISR on every 0.5 s tick. */
volatile __data uint8_t g_tick = 0;
/** @brief Set by Port 1 ISR on PIR edge. */
volatile __data uint8_t g_pir_edge = 0;

/* ============================================================================
 * Interrupt Service Routines (PM2 wake-up sources) - kept minimal
 * ============================================================================ */

/** @brief Sleep Timer Event0 (Vector 5). */
void st_isr(void) __interrupt(ST_VECTOR) {
    SLEEP &= ~0x03;   // Cancel pending PM entry
    WORIRQ &= ~0x01;  // Clear EVENT0_FLAG (keep EVENT0_MASK)
    STIF = 0;
    g_tick = 1;
}

/** @brief Port 1 (Vector 15) - PIR ALARM on P1_7. PxIFG must be cleared before PxIF. */
void p1_isr(void) __interrupt(P1INT_VECTOR) {
    SLEEP &= ~0x03;
    P1IFG = 0x00;
    P1IF = 0;
    g_pir_edge = 1;
}

volatile uint8_t g_uart_dummy;

/** @brief USART1 RX Complete (Vector 3) - captures Blackfin UART responses without dropping bytes. */
void urx1_isr(void) __interrupt(URX1_VECTOR) {
    URX1IF = 0;
    if (g_uart_rx_count < 64) {
        g_uart_rx_buf[g_uart_rx_count++] = U1DBUF;
    } else {
        g_uart_dummy = U1DBUF; // Strikt tilldelning till volatile så SDCC genererar MOV-instruktion
    }
}

/* ============================================================================
 * Delays
 * ============================================================================ */

/** @brief Busy-wait ms delay calibrated for 27 MHz (takes 2x longer on 13.5 MHz RCOSC). */
void delay_ms(uint16_t ms) {
    while (ms--) {
        for (uint16_t i = 0; i < 2180; i++) {
            __asm nop __endasm;
        }
    }
}

/* ============================================================================
 * AES-128 Coprocessor
 * ============================================================================ */

void aes_load_key(const uint8_t __code *key) {
    uint16_t timeout = 50000;
    ENCCS = (0x02 << 1) | 0x01; // CMD = Load Key, ST = 1
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = key[i];
    }
    while (!(ENCCS & 0x08) && --timeout);
}

void aes_encrypt_block(const uint8_t *in, uint8_t *out) {
    uint16_t timeout = 50000;
    ENCCS = 0x40 | 0x01; // MODE = ECB, CMD = Encrypt, ST = 1
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = in[i];
    }
    while (!(ENCCS & 0x08) && --timeout);
    for (uint8_t i = 0; i < 16; i++) {
        out[i] = ENCDO;
    }
}

/* ============================================================================
 * Battery (VDD/3)
 * ============================================================================ */

uint16_t read_battery_mv(void) {
    ADCCON3 = 0x3F; // Internal 1.25V ref, 12-bit, VDD/3
    uint16_t timeout = 5000;
    while (!(ADCCON1 & 0x80) && --timeout);

    uint16_t adc = ((uint16_t)ADCL >> 4) | ((uint16_t)ADCH << 4);
    return (uint16_t)(((uint32_t)adc * 3750UL) / 2047UL);
}

uint8_t calculate_battery_pct(uint16_t mv) {
    if (mv >= 3000) return 100;
    if (mv <= 2200) return 0;
    return (uint8_t)(((uint32_t)(mv - 2200) * 100UL) / 800UL);
}

/* ============================================================================
 * Clock switching: 27 MHz XOSC (TX) <-> 13.5 MHz HS RCOSC (required before PM2)
 * ============================================================================ */

/** @brief Run on 27.000 MHz crystal Y1 (needed for radio). */
static void clock_xosc(void) {
    uint16_t timeout;
    SLEEP &= ~0x04;                               // Power up both HS oscillators
    timeout = 50000;
    while (!(SLEEP & 0x40) && --timeout);         // XOSC_STB
    CLKCON = (CLKCON & 0x80);                     // OSC=0 (XOSC), TICKSPD=0, CLKSPD=0
    timeout = 50000;
    while ((CLKCON & 0x40) && --timeout);
    SLEEP |= 0x04;                                // Power down unused HS RCOSC
}

/** @brief Run on 13.5 MHz HS RCOSC and power down the 27 MHz crystal. */
static void clock_rcosc(void) {
    uint16_t timeout;
    RFST = 0x04;                                  // Radio SIDLE
    SLEEP &= ~0x04;
    timeout = 50000;
    while (!(SLEEP & 0x20) && --timeout);         // HFRC_STB
    CLKCON = (CLKCON & 0x80) | 0x40 | 0x08 | 0x01; // OSC=1, TICKSPD=001, CLKSPD=001
    timeout = 50000;
    while (!(CLKCON & 0x40) && --timeout);
    SLEEP |= 0x04;                                // Power down unused 27 MHz XOSC
}

volatile __xdata uint8_t g_dsp_rx[4] = {0, 0, 0, 0};
volatile __xdata uint8_t g_image_chunk[64];
__xdata uint8_t g_spi_rx_buf[2048];

/* ============================================================================
 * UART1 (USART1 Alt. 1: P0_4=TX, P0_5=RX @ 57600 baud)
 * ============================================================================ */

void uart1_init(uint8_t baud_e, uint8_t baud_m) {
    PERCFG &= ~0x02;     // U1CFG = 0 (Alt. 1: P0_4=TX, P0_5=RX)
    P0SEL |= 0x30;       // Peripheral function on P0_4 and P0_5
    P0DIR |= 0x10;       // P0_4 = Output (TX)
    P0DIR &= ~0x20;      // P0_5 = Input (RX)
    P0INP &= ~0x30;      // Enable pull-up on P0_5
    P2INP &= ~0x20;      // Port 0 pull-UP mode

    U1CSR = 0x80 | 0x40; // MODE=1 (UART), RE=1 (Receive enable)
    U1UCR = 0x02;        // 8N1, high stop bit
    U1GCR = baud_e;
    U1BAUD = baud_m;
}

void uart1_init_57600(void) {
    uart1_init(0x0B, 0x18); // 57600 @ 27 MHz
    URX1IF = 0;
    URX1IE = 1;             // Enable UART1 RX interrupt
    EA = 1;                 // Global interrupt enable
}

void uart1_init_115200(void) {
    uart1_init(0x0C, 0x18); // 115200 @ 27 MHz
    URX1IF = 0;
    URX1IE = 1;             // Enable UART1 RX interrupt
    EA = 1;                 // Global interrupt enable
}

uint8_t uart1_rx_ready(void) {
    return (U1CSR & 0x04) != 0; // URX1IF
}

uint8_t uart1_rx_byte(void) {
    U1CSR &= ~0x04;
    return U1DBUF;
}

void uart1_tx_byte(uint8_t b) {
    uint16_t timeout = 10000;
    U1DBUF = b;
    while (!(U1CSR & 0x02) && --timeout);
    U1CSR &= ~0x02;
}

void uart1_tx_str(const char *str) {
    while (*str) {
        uart1_tx_byte((uint8_t)*str++);
    }
}

static void spi_read_jedec_id(uint8_t *id) {
    uint8_t i, b;
    uint8_t cmd = 0x9F; // Read JEDEC ID

    P1DIR |= 0x28;  // P1_3 (SCK) and P1_5 (MOSI) = Outputs
    P1DIR &= ~0x10; // P1_4 (MISO) = Input
    P1INP &= ~0x10; // Enable pull-up on P1_4
    P1_3 = 0;       // SCK idle low

    // Send command 0x9F
    for (b = 0; b < 8; b++) {
        P1_5 = (cmd & 0x80) ? 1 : 0;
        cmd <<= 1;
        P1_3 = 1;
        P1_3 = 0;
    }

    // Read 3 ID bytes
    for (i = 0; i < 3; i++) {
        uint8_t val = 0;
        for (b = 0; b < 8; b++) {
            P1_3 = 1;
            val = (val << 1) | (P1_4 ? 1 : 0);
            P1_3 = 0;
        }
        id[i] = val;
    }
}

/* ============================================================================
 * SPI0 Master (USART0 Alt. 2: P1_3=SCK, P1_4=MISO, P1_5=MOSI; P1_2=SS1, P1_1=SS2)
 * ============================================================================ */

void spi0_master_init(void) {
    PERCFG |= 0x01;      // Route USART0 to Alternative 2 (P1_2..P1_5)
    P1SEL |= 0x38;       // P1_3 (SCK), P1_4 (MISO), P1_5 (MOSI) = Peripheral function
    P1SEL &= ~0x06;      // P1_1 (Bus Switch) & P1_2 (SS) = GPIO outputs
    P1DIR |= 0x2E;       // P1_1, P1_2, P1_3, P1_5 = Outputs
    P1DIR &= ~0x10;      // P1_4 (MISO) = Input
    P1INP &= ~0x10;      // Pull-up on P1_4

    P1_3 = 1;            // SCK idle HIGH i Mode 3!
    U0CSR = 0x00;        // SPI Master mode
    // Mode 3: CPOL=1, CPHA=1 (0xC0), MSB-first (0x20) -> 0xE0
    // BAUD_E = 13, BAUD_M = 173 -> Exakt 353.5 kHz (Verisure originalfrekvens från Kort #2!)
    U0GCR = 0xE0 | (13 & 0x1F);
    U0BAUD = 173;
}

void spi0_master_config(uint8_t mode, uint8_t baud_e) {
    uint8_t gcr = 0x20 | (baud_e & 0x1F); // ORDER=1 (MSB first)
    PERCFG |= 0x01;      // Ensure Alt. 2
    P1SEL |= 0x38;
    if (mode == 0) {
        P1_3 = 0; // Mode 0: CPOL=0, CPHA=0
    } else if (mode == 1) {
        P1_3 = 0; // Mode 1: CPOL=0, CPHA=1
        gcr |= 0x40;
    } else if (mode == 2) {
        P1_3 = 1; // Mode 2: CPOL=1, CPHA=0
        gcr |= 0x80;
    } else {
        P1_3 = 1; // Mode 3: CPOL=1, CPHA=1
        gcr |= 0xC0;
    }
    U0GCR = gcr;
    U0BAUD = 0x00;
}

uint8_t spi0_master_txrx(uint8_t b) {
    uint16_t timeout = 10000;
    URX0IF = 0;
    U0CSR |= 0x40; // RE = 1
    U0DBUF = b;
    while (!URX0IF && --timeout);
    URX0IF = 0;
    return U0DBUF;
}

uint16_t spi0_transfer_word16(uint16_t out_word) {
    uint8_t hi, lo;

    P1_1 = 1; // Bus Switch MÅSTE förbli HÖG (Aktiv) för att koppla bussen!
    P1_2 = 0; // Assert Blackfin /SPISS
    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }

    hi = spi0_master_txrx((uint8_t)(out_word >> 8));
    lo = spi0_master_txrx((uint8_t)(out_word & 0xFF));

    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }
    P1_2 = 1; // Deassert Blackfin /SPISS
    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }

    return ((uint16_t)hi << 8) | lo;
}

/* ============================================================================
 * GPIO
 * ============================================================================ */

void setup_gpio(void) {
    // Port 0 & Port 1 baskonfiguration
    P0SEL = 0x00;
    P1SEL = 0x00;
    P2SEL &= ~0x07;

    // CC1110 SPI-pinnar hålls i 3-state så att U3 Flash inte belastas
    P1DIR &= ~0x38;
    P1INP |= 0x38;

    // Alla styr- och kandidatpinnar på Port 0 och Port 2 sätts som utgångar och drivs LÅGA (0V)
    // för att släcka eventuella pull-ups som driver transistorn Q2 till den röda dioden LD1:
    P0DIR |= 0xCF;  // P0_0, P0_1, P0_2, P0_3, P0_6, P0_7 som utgångar
    P0 &= ~0xCF;    // Alla drivs till 0.0 V
    P2DIR |= 0x01;  // P2_0 som utgång
    P2_0 = 0;       // Drivs till 0.0 V

    P1DIR |= 0x07;  // P1_0 (3.3V), P1_1 (Bus Switch), P1_2 (SS) = Utgångar
    P1_0 = 0;       // 3.3V Switched rail OFF
    P1_1 = 0;       // Bus Switch frånkopplad
    P1_2 = 1;       // SS idle HIGH

    // Håll knappen SW3 (P1_6) och PIR (P1_7) strikt som ingångar med pull-UP:
    P1DIR &= ~0xC0;
    P1INP &= ~0xC0; // Enable pull
    P2INP &= ~0x40; // Port 1 pull-UP (PIR P1_7 kräver pull-up!)
}

/**
 * @brief Dual pull-up / pull-down scan of the candidate switch pins.
 * Masks: P0 0xC1 (P0_7, P0_6, P0_0), P1 0xC0 (P1_7, P1_6), P2 0x01 (P2_0).
 */
static void read_gpio_scan(uint8_t *p0_pu, uint8_t *p0_pd, uint8_t *p1_pu, uint8_t *p1_pd, uint8_t *p2_packed) {
    uint8_t d;

    P2INP &= ~0xE0; // All ports pull-UP
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }
    *p0_pu = P0;
    *p1_pu = P1;
    uint8_t p2_u = P2;

    P2INP |= 0xE0;  // All ports pull-DOWN
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }
    *p0_pd = P0;
    *p1_pd = P1;
    uint8_t p2_d = P2;

    P2INP = (P2INP & ~0xC0) | 0x20; // Restore default
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }

    *p2_packed = p2_u;
}

static bool read_tamper(void) {
    uint8_t d;
    bool tamper;

    // Set Port 0 to pull-DOWN mode (P2INP bit 5 = 1)
    P2INP |= 0x20;
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }

    // SW3 connects P0_6 to VBAT+. If pressed/shorted -> P0_6 is HIGH (Secure).
    // If open/disconnected -> pull-down pulls P0_6 to LOW (Tamper Alarm).
    tamper = ((P0 & 0x40) == 0);

    // Restore Port 0 to pull-UP mode (P2INP bit 5 = 0)
    P2INP &= ~0x20;
    for (d = 0; d < 20; d++) {
        __asm nop __endasm;
    }

    return tamper;
}

/* ============================================================================
 * Radio (868.950 MHz @ 27.000 MHz crystal Y1)
 * ============================================================================ */

void radio_init(void) {
    RFST = 0x04; // SIDLE
    delay_ms(1);

    SYNC1 = 0xD3;
    SYNC0 = 0x91;

    PKTLEN   = PACKET_LEN;
    PKTCTRL1 = 0x04;
    PKTCTRL0 = 0x04;

    CHANNR  = 0x00;
    FSCTRL1 = 0x06;
    FSCTRL0 = 0x00;

    FREQ2 = 0x20; // 868.950 MHz (0x202EEF)
    FREQ1 = 0x2E;
    FREQ0 = 0xEF;

    MDMCFG4 = 0x8A; // 38.383 kBaud, 2-FSK
    MDMCFG3 = 0x75;
    MDMCFG2 = 0x03;
    MDMCFG1 = 0x22;
    MDMCFG0 = 0xF8;
    DEVIATN = 0x35;

    MCSM2 = 0x07;
    MCSM1 = 0x00;
    MCSM0 = 0x18;

    FOCCFG = 0x16;
    BSCFG  = 0x6C;

    AGCCTRL2 = 0x43;
    AGCCTRL1 = 0x40;
    AGCCTRL0 = 0x91;

    FREND1 = 0x56;
    FREND0 = 0x10;

    FSCAL3 = 0xE9;
    FSCAL2 = 0x2A;
    FSCAL1 = 0x00;
    FSCAL0 = 0x1F; // Charge pump calibration must remain 0x1F

    TEST2 = 0x81;
    TEST1 = 0x35;
    TEST0 = 0x09;

    PA_TABLE0 = 0xC2; // +10 dBm
    PA_TABLE1 = 0xC2;
    PA_TABLE2 = 0xC2;
    PA_TABLE3 = 0xC2;
    PA_TABLE4 = 0xC2;
    PA_TABLE5 = 0xC2;
    PA_TABLE6 = 0xC2;
    PA_TABLE7 = 0xC2;
}

bool radio_send_dma(void) {
    uint16_t timeout;

    RFST = 0x04; // SIDLE
    timeout = 5000;
    while ((MARCSTATE & 0x1F) != 0x01 && --timeout);

    dma_desc.src_h = (uint8_t)(((uint16_t)tx_buf) >> 8);
    dma_desc.src_l = (uint8_t)(((uint16_t)tx_buf) & 0xFF);
    dma_desc.dst_h = 0xDF;
    dma_desc.dst_l = 0xD9;       // X_RFD
    dma_desc.len_h = 0x00;
    dma_desc.len_l = PACKET_LEN;
    dma_desc.cfg0  = 19;         // DMA_TRIG_RADIO
    dma_desc.cfg1  = 0x4A;

    DMA0CFGH = (uint8_t)(((uint16_t)&dma_desc) >> 8);
    DMA0CFGL = (uint8_t)(((uint16_t)&dma_desc) & 0xFF);

    RFTXRXIF = 0;
    RFIF = 0;
    DMAIRQ &= ~0x01;

    DMAARM = 0x01;
    __asm
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
    __endasm;

    RFST = 0x03; // STX

    timeout = 50000;
    while (!(RFIF & 0x10) && --timeout) {
        if ((MARCSTATE & 0x1F) == 0x16) {
            RFST = 0x04;
            RFIF = 0;
            return false;
        }
    }

    bool done = (DMAIRQ & 0x01) != 0 || (RFIF & 0x10) != 0;

    RFST = 0x04;
    RFTXRXIF = 0;
    RFIF = 0;

    return (timeout > 0 && done);
}

/* ============================================================================
 * Sleep Timer + PM2
 * ============================================================================ */

/** @brief Start the periodic 0.5 s Sleep Timer Event0 (runs on 32 kHz RCOSC). */
static void sleep_timer_init(void) {
    uint8_t t0;
    uint16_t timeout;

    WORCTRL = 0x04 | ST_WOR_RES; // WOR_RESET + resolution

    // Wait for 2 sleep-timer edges after reset (SWRS033), bounded so it can never hang
    t0 = WORTIME0;
    timeout = 60000;
    while (WORTIME0 == t0 && --timeout);
    t0 = WORTIME0;
    timeout = 60000;
    while (WORTIME0 == t0 && --timeout);

    WOREVT1 = 0x00;
    WOREVT0 = ST_WOREVT;

    WORIRQ = 0x10; // EVENT0_MASK = 1, clear flag
    STIF = 0;
    STIE = 1;
}

/**
 * @brief Enter PM2 until the Sleep Timer tick or a PIR edge on P1_7.
 * @pre CPU runs on HS RCOSC (clock_rcosc()).
 */
static void enter_pm2(void) {
    uint8_t pir_level;
    uint8_t d;

    EA = 0;

    // Arm P1_7 for the edge AWAY from the current level (P1ICON = PICTL bit 1)
    pir_level = P1_7;
    if (pir_level) {
        PICTL |= 0x02;  // Falling edge: motion starts
    } else {
        PICTL &= ~0x02; // Rising edge: motion ends
    }
    P1IEN = 0x80;
    P1IFG = 0x00;
    P1IF = 0;
    IEN2 |= 0x10;       // P1IE

    // Race check: level changed while arming, or a tick is already pending
    if (P1_7 != pir_level || g_tick || g_pir_edge) {
        EA = 1;
        return;
    }

    // Give at least one 32 kHz period between wake-up and re-entering PM2
    for (d = 0; d < 200; d++) {
        __asm nop __endasm;
    }

    SLEEP = (SLEEP & ~0x03) | 0x02; // MODE = PM2
    __asm
        nop
        nop
        nop
    __endasm;

    EA = 1;
    if (SLEEP & 0x03) {   // An ISR clears MODE if it fired in the meantime
        PCON |= 0x01;     // Enter PM2
        __asm nop __endasm;
    }
}

/* ============================================================================
 * Packet build + transmit
 * ============================================================================ */

static void transmit(uint16_t seq, bool is_motion, bool is_tamper, bool is_por,
                     uint8_t p0_pu, uint8_t p0_pd, uint8_t p1_pu, uint8_t p2_packed) {
    uint8_t plain[16];
    uint16_t batt_mv;
    uint8_t flags = 0;

    clock_xosc();
    radio_init();
    aes_load_key(DEVICE_AES_KEY);

    batt_mv = read_battery_mv();

    for (uint8_t i = 0; i < 8; i++) {
        tx_buf[i] = (uint8_t)DEVICE_ID[i];
    }

    if (is_motion) flags |= 0x01;           // Bit 0: Motion
    if (is_tamper) flags |= 0x04;           // Bit 2: Tamper
    if (is_por)    flags |= 0x08;           // Bit 3: Power-on reset
    if (batt_mv < 2400) flags |= 0x10;      // Bit 4: Low battery

    plain[0]  = (uint8_t)(seq >> 8);
    plain[1]  = (uint8_t)(seq & 0xFF);
    plain[2]  = flags;
    plain[3]  = calculate_battery_pct(batt_mv);
    plain[4]  = (uint8_t)(batt_mv >> 8);
    plain[5]  = (uint8_t)(batt_mv & 0xFF);
    plain[6]  = (uint8_t)DEVICE_ID[0];
    plain[7]  = (uint8_t)DEVICE_ID[1];
    plain[8]  = (uint8_t)DEVICE_ID[2];
    plain[9]  = (uint8_t)DEVICE_ID[3];
    plain[10] = 'V';
    plain[11] = 'S';
    plain[12] = p0_pu;
    plain[13] = p0_pd;
    plain[14] = p1_pu;
    plain[15] = p2_packed;

    aes_encrypt_block(plain, &tx_buf[8]);

    g_tx_count++;
    if (!radio_send_dma()) {
        g_tx_fail++;
    }

    // Keep 27 MHz crystal running for high-speed SPI with Blackfin DSP
    // clock_rcosc();
}

/* ============================================================================
 * Main
 * ============================================================================ */

static uint8_t scan_cmd(uint8_t ss_sel, uint8_t cmd) {
    uint8_t resp = 0;

    // Assert chosen SS
    if (ss_sel == 1) {
        P1_2 = 0; // Assert P1_2 (SS1 - Upper)
    } else if (ss_sel == 2) {
        P1_1 = 0; // Assert P1_1 (SS2 - Lower)
    } else {
        P1_2 = 0;
        P1_1 = 0; // Both
    }
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    spi0_master_txrx(cmd);
    resp = spi0_master_txrx(0x00);
    spi0_master_txrx(0x00);
    spi0_master_txrx(0x00);

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    // Deassert SS
    P1_2 = 1;
    P1_1 = 1;
    delay_ms(10);

    return resp;
}


static void spi_set_mode(uint8_t mode) {
    if (mode == 0) {
        // Mode 0: CPOL=0, CPHA=0, SCK idle LOW
        P1_3 = 0;
        U0GCR = 0x20 | 0x0F; // ORDER=1 (MSB), CPOL=0, CPHA=0, BAUD_E=15
    } else {
        // Mode 3: CPOL=1, CPHA=1, SCK idle HIGH
        P1_3 = 1;
        U0GCR = 0xE0 | 0x0F; // ORDER=1 (MSB), CPOL=1, CPHA=1, BAUD_E=15
    }
}

static uint16_t spi_txrx_16bit(uint16_t word) {
    uint8_t hi = spi0_master_txrx((uint8_t)(word >> 8));
    uint8_t lo = spi0_master_txrx((uint8_t)(word & 0xFF));
    return ((uint16_t)hi << 8) | lo;
}

static uint16_t test_dsp_16bit(uint8_t ss_pin, uint8_t spi_mode, uint16_t cmd) {
    uint16_t resp = 0;

    spi_set_mode(spi_mode);

    // Assert Chip Select (drive both SS1 and SS2 low if needed so both Saleae and DSP see it)
    if (ss_pin == 1) {
        P1_2 = 0;
    } else if (ss_pin == 2) {
        P1_1 = 0;
    } else {
        P1_2 = 0;
        P1_1 = 0;
    }
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    // Send 16-bit command word
    spi_txrx_16bit(cmd);
    // Read 16-bit response word
    resp = spi_txrx_16bit(0x0000);

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    // Deassert Chip Select
    P1_2 = 1;
    P1_1 = 1;
    delay_ms(10);

    return resp;
}

static int16_t read_camera_stream(uint8_t ss_pin, uint8_t spi_mode, uint16_t cmd) {
    int16_t jpeg_offset = -1;

    spi_set_mode(spi_mode);

    // 1. Assert SS and send command to trigger snapshot / query stream
    if (ss_pin == 1) P1_2 = 0;
    else if (ss_pin == 2) P1_1 = 0;
    else { P1_2 = 0; P1_1 = 0; }
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    spi_txrx_16bit(cmd);

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_2 = 1;
    P1_1 = 1;

    // 2. Wait 600 ms for Sunny camera exposure and Blackfin JPEG compression
    delay_ms(600);

    // 3. Assert SS to read out stream
    if (ss_pin == 1) P1_2 = 0;
    else if (ss_pin == 2) P1_1 = 0;
    else { P1_2 = 0; P1_1 = 0; }
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    // Read 256 bytes from Blackfin DSP
    for (uint16_t i = 0; i < 256; i++) {
        g_spi_rx_buf[i] = spi0_master_txrx(0x00);
    }

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_2 = 1;
    P1_1 = 1;

    // Search for JPEG Start-Of-Image marker (0xFF 0xD8)
    for (uint16_t i = 0; i < 255; i++) {
        if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i+1] == 0xD8) {
            jpeg_offset = (int16_t)i;
            break;
        }
    }

    return jpeg_offset;
}

static int16_t trigger_and_read_image(uint16_t cmd, uint8_t p16_level) {
    int16_t jpeg_offset = -1;

    // Set hardware trigger pin P1_6
    P1_6 = p16_level;
    delay_ms(10);

    // Ensure bus switch is enabled
    P1_1 = 1;

    // 1. Assert Blackfin /SPISS (P1_2 = 0)
    P1_2 = 0;
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    // Send 16-bit command to Blackfin DSP
    spi_txrx_16bit(cmd);

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_2 = 1;

    // Send UART trigger in parallel
    uart1_tx_byte((uint8_t)(cmd >> 8));
    uart1_tx_byte((uint8_t)(cmd & 0xFF));
    uart1_tx_str("snap\r\n");

    // 2. Wait 500 ms for Sunny camera exposure and Blackfin JPEG compression
    delay_ms(500);

    // 3. Assert Blackfin /SPISS to read out JPEG stream
    P1_2 = 0;
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    // Read 512 bytes from Blackfin DSP
    for (uint16_t i = 0; i < 512; i++) {
        g_spi_rx_buf[i] = spi0_master_txrx(0x00);
    }

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_2 = 1;

    // Search for JPEG Start-Of-Image marker (0xFF 0xD8)
    for (uint16_t i = 0; i < 511; i++) {
        if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i+1] == 0xD8) {
            jpeg_offset = (int16_t)i;
            break;
        }
    }

    return jpeg_offset;
}

static void sweep_read_commands(void) {
    const uint8_t read_cmds[8] = {0x03, 0x3D, 0x81, 0x84, 0x02, 0x04, 0x01, 0x9F};

    for (uint8_t c = 0; c < 8; c++) {
        uint8_t cmd = read_cmds[c];

        // Assert Blackfin /SPISS
        P1_2 = 0;
        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

        // Send candidate read command byte
        spi0_master_txrx(cmd);
        // Send 3 dummy address/control bytes
        spi0_master_txrx(0x00);
        spi0_master_txrx(0x00);
        spi0_master_txrx(0x00);

        // Clock out 64 bytes
        for (uint8_t i = 0; i < 64; i++) {
            g_spi_rx_buf[i] = spi0_master_txrx(0x00);
        }

        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
        // Deassert /SPISS
        P1_2 = 1;
        delay_ms(50);
    }
}

static uint8_t test_single_cs(uint8_t pin_code) {
    // Release all candidate pins HIGH
    P1_2 = 1;
    P1_1 = 1;
    P1_6 = 1;
    P0_0 = 1;
    delay_ms(10);

    // Assert chosen pin(s)
    if (pin_code == 1) P1_2 = 0;                      // P1_2 only
    else if (pin_code == 2) P1_1 = 0;                 // P1_1 only
    else if (pin_code == 3) P1_6 = 0;                 // P1_6 only
    else if (pin_code == 4) P0_0 = 0;                 // P0_0 only (Blackfin Reset)
    else if (pin_code == 5) { P1_2 = 0; P0_0 = 0; }   // P1_2 + P0_0
    else if (pin_code == 6) { P1_1 = 0; P0_0 = 0; }   // P1_1 + P0_0
    else if (pin_code == 7) { P1_2 = 0; P1_1 = 0; }   // P1_2 + P1_1
    else if (pin_code == 8) { P1_6 = 0; P0_0 = 0; }   // P1_6 + P0_0

    for (uint8_t d = 0; d < 50; d++) { __asm nop __endasm; }

    // Send 0x9F (Read JEDEC ID)
    spi0_master_txrx(0x9F);
    uint8_t b0 = spi0_master_txrx(0x00);
    uint8_t b1 = spi0_master_txrx(0x00);
    uint8_t b2 = spi0_master_txrx(0x00);

    for (uint8_t d = 0; d < 50; d++) { __asm nop __endasm; }

    // Release all
    P1_2 = 1;
    P1_1 = 1;
    P1_6 = 1;
    P0_0 = 1;
    delay_ms(10);

    // Check if 0x1C 0x31 0x13 returned
    if (b0 == 0x1C && b1 == 0x31 && b2 == 0x13) {
        return 1; // MATCH!
    }
    return 0;
}

static __xdata uint8_t s_img_plain[16];
static __xdata uint8_t s_img_chunk[8];
static __xdata uint32_t s_flash_addr;
static __xdata uint32_t s_flash_len;
static __xdata uint32_t s_found_addr;

static void transmit_image_chunk(uint16_t seq, uint16_t chunk_idx, uint8_t chunk_len,
                                 bool is_first, bool is_last, const uint8_t *data) {
    uint8_t i;

    aes_load_key(DEVICE_AES_KEY);

    for (i = 0; i < 8; i++) {
        tx_buf[i] = (uint8_t)DEVICE_ID[i];
    }

    s_img_plain[0] = (uint8_t)(seq >> 8);
    s_img_plain[1] = (uint8_t)(seq & 0xFF);
    s_img_plain[2] = 0x80; // Image chunk packet flag
    s_img_plain[3] = (uint8_t)(chunk_idx >> 8);
    s_img_plain[4] = (uint8_t)(chunk_idx & 0xFF);

    uint8_t flags = chunk_len & 0x0F;
    if (is_first) flags |= 0x80;
    if (is_last)  flags |= 0x40;
    s_img_plain[5] = flags;

    for (i = 0; i < 8; i++) {
        s_img_plain[6 + i] = (i < chunk_len) ? data[i] : 0x00;
    }

    s_img_plain[14] = 'V';
    s_img_plain[15] = 'S';

    aes_encrypt_block(s_img_plain, &tx_buf[8]);

    radio_send_dma();
}



static uint32_t scan_flash_for_jpeg(uint32_t *out_jpeg_len) {
    uint8_t prev = 0;
    s_found_addr = 0xFFFFFFFF;
    s_flash_len = 0;

    P0_0 = 0; // Hold Blackfin in RESET
    P1_1 = 1; // Bus Switch ON
    spi0_master_init();
    P1DIR |= 0x40; // P1_6 = Output (/CS to U3 Flash)
    P1_6 = 1;      // Idle HIGH
    delay_ms(10);

    // Read JEDEC ID first to verify contact
    P1_6 = 0;
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    spi0_master_txrx(0x9F);
    uint8_t id0 = spi0_master_txrx(0x00);
    uint8_t id1 = spi0_master_txrx(0x00);
    uint8_t id2 = spi0_master_txrx(0x00);
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_6 = 1;
    delay_ms(5);

    if (id0 != 0x1C) {
        *out_jpeg_len = 0;
        return 0xFFFFFFFF;
    }

    // Now start Read Data (0x03) from address 0x000000
    P1_6 = 0;
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    spi0_master_txrx(0x03);
    spi0_master_txrx(0x00);
    spi0_master_txrx(0x00);
    spi0_master_txrx(0x00);

    for (s_flash_addr = 0; s_flash_addr < 0x080000; s_flash_addr++) {
        uint8_t b = spi0_master_txrx(0x00);
        if (s_found_addr == 0xFFFFFFFF) {
            if (prev == 0xFF && b == 0xD8) {
                s_found_addr = s_flash_addr - 1;
            }
        } else {
            if (prev == 0xFF && b == 0xD9) {
                s_flash_len = (s_flash_addr - s_found_addr) + 1;
                break;
            }
        }
        prev = b;
    }

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_6 = 1;

    *out_jpeg_len = s_flash_len;
    return s_found_addr;
}

static void stream_flash_range(uint32_t start_addr, uint32_t length, uint16_t *p_seq) {
    uint32_t offset = 0;
    uint16_t chunk_idx = 0;

    clock_xosc();
    radio_init();
    delay_ms(5);

    while (offset < length) {
        uint8_t chunk_len = 0;
        uint32_t cur_addr = start_addr + offset;

        P1_6 = 0; // Assert U3 /CS
        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

        spi0_master_txrx(0x03);
        spi0_master_txrx((uint8_t)(cur_addr >> 16));
        spi0_master_txrx((uint8_t)(cur_addr >> 8));
        spi0_master_txrx((uint8_t)(cur_addr & 0xFF));

        while (chunk_len < 8 && (offset + chunk_len) < length) {
            s_img_chunk[chunk_len++] = spi0_master_txrx(0x00);
        }

        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
        P1_6 = 1;

        bool is_first = (chunk_idx == 0);
        bool is_last = (offset + chunk_len >= length);

        transmit_image_chunk((*p_seq)++, chunk_idx++, chunk_len, is_first, is_last, s_img_chunk);
        offset += chunk_len;

        delay_ms(15); // Clean 15 ms pacing between RF packets
    }
}

static void stream_buffer_rf(const uint8_t *buf, uint16_t len, uint16_t *p_seq);

static void test_blackfin_boot_uart(uint16_t *p_seq) {
    uint8_t rx_buf[16];
    uint8_t rx_count = 0;
    uint16_t timeout;

    // Strömsätt rails med Blackfin i RESET
    P0DIR |= 0x85;
    P1DIR |= 0x07;
    P0_0 = 0; // RESET
    P0_2 = 1; // 1.2V
    P0_7 = 1; // Aux
    P1_0 = 1; // 3.3V
    P1_1 = 1; // Bus Switch

    // Tristatera SPI-pinnar så Blackfin kan ladda U3 Flash
    P1SEL &= ~0x38;
    P1DIR &= ~0x38;
    P1INP &= ~0x38;

    // Konfigurera UART1 Alt 1 (P0_4=TX, P0_5=RX)
    PERCFG &= ~0x02; // Alt 1
    P0SEL |= 0x30;   // Peripheral funktion på P0_4 & P0_5
    P0DIR |= 0x10;   // P0_4 = Utgång (TX)
    P0DIR &= ~0x20;  // P0_5 = Ingång (RX)
    P0INP &= ~0x20;  // Pull-up på P0_5

    U1CSR = 0x80 | 0x40; // UART Mode, RE=1
    U1GCR = 11;
    U1BAUD = 216; // 57600 @ 27 MHz

    delay_ms(150);

    // Släpp Blackfin ur reset!
    P0_0 = 1;

    // Lyssna i 1 sekund vid 57600 baud
    for (timeout = 0; timeout < 1000; timeout++) {
        if (U1CSR & 0x04) {
            uint8_t b = U1DBUF;
            if (rx_count < 16) rx_buf[rx_count++] = b;
        }
        delay_ms(1);
    }

    // Om inga bytes vid 57600, prova lyssna vid 115200 baud
    if (rx_count == 0) {
        U1GCR = 12;
        U1BAUD = 216; // 115200 @ 27 MHz
        // Skicka trigger-sträng
        U1DBUF = 'S'; while (!(U1CSR & 0x02));
        U1DBUF = 'N'; while (!(U1CSR & 0x02));
        U1DBUF = 'A'; while (!(U1CSR & 0x02));
        U1DBUF = 'P'; while (!(U1CSR & 0x02));
        U1DBUF = '\r'; while (!(U1CSR & 0x02));
        U1DBUF = '\n'; while (!(U1CSR & 0x02));

        for (timeout = 0; timeout < 500; timeout++) {
            if (U1CSR & 0x04) {
                uint8_t b = U1DBUF;
                if (rx_count < 16) rx_buf[rx_count++] = b;
            }
            delay_ms(1);
        }
    }

    // Annonsera resultat via telemetri:
    // opcode 0xCC, rx_count, rx_buf[0], rx_buf[1]
    transmit((*p_seq)++, true, false, false, 0xCC, rx_count, 
             rx_count > 0 ? rx_buf[0] : 0, 
             rx_count > 1 ? rx_buf[1] : 0);
    delay_ms(50);
}

static void stream_code_image_rf(uint16_t *p_seq) {
    uint16_t offset = 0;
    uint16_t chunk_idx = 0;
    uint8_t chunk[8];
    uint8_t i;

    clock_xosc();
    radio_init();
    delay_ms(5);

    while (offset < CAMERA_IMAGE_LEN) {
        uint8_t chunk_len = (CAMERA_IMAGE_LEN - offset > 8) ? 8 : (uint8_t)(CAMERA_IMAGE_LEN - offset);
        bool is_first = (chunk_idx == 0);
        bool is_last = (offset + chunk_len >= CAMERA_IMAGE_LEN);

        for (i = 0; i < chunk_len; i++) {
            chunk[i] = CAMERA_IMAGE_DATA[offset + i]; // Strikt MOVC-läsning från Flash ROM
        }

        transmit_image_chunk((*p_seq)++, chunk_idx++, chunk_len, is_first, is_last, chunk);
        offset += chunk_len;

        delay_ms(15);
    }
}

static int16_t capture_blackfin_jpeg(uint16_t *out_len, uint16_t *p_seq) {
    int16_t jpeg_start = -1;
    uint16_t i;
    uint16_t total_len = 0;

    *out_len = 0;

    // STEG 1: Strömsätt Blackfin och sensorskenor med Blackfin i RESET
    P0DIR |= 0x85; // P0_0 (/RESET), P0_2 (1.2V), P0_7 (Aux) = Utgångar
    P1DIR |= 0x07; // P1_0 (3.3V), P1_1 (Bus Switch), P1_2 (DSP /SS) = Utgångar
    P1DIR &= ~0x40; // P1_6 är SW3 Tamper - HÅLL ALLTID SOM INGÅNG!
    P1INP &= ~0x40;

    // Håll P1_7 som ingång (PIR)
    P1DIR &= ~0x80;
    P1INP &= ~0x80;

    // STEG 1: Nollställ först eventuell restspänning på skenorna (äkta ren kallstart)
    P0_0 = 0; // Håll Blackfin i RESET
    P0_2 = 0; // 1.2V Core rail OFF
    P0_7 = 0; // Aux rail OFF
    P1_0 = 0; // 3.3V switched OFF
    P1_1 = 0; // Bus switch OFF
    P1_2 = 1; // DSP /SS idle HIGH
    delay_ms(200); // Ladda ur kondensatorer så kamerasensorn och SDRAM nollställs

    // Nu: Strömsätt kameran och Blackfin (RÖDA DIODEN TÄNDS!)
    P0_2 = 1;      // 1.2V Core rail (Q13) ON
    P0_7 = 1;      // 3.27V aux/sensor analog rail ON (Röda dioden lyser!)
    P1_0 = 1;      // 3.3V switched rail (Q7 - SDRAM & Sensor) ON

    // Sätt CC1110 SPI-pinnar i äkta 3-state (P1INP bit=1 stänger av pull-resistorer!)
    P1SEL &= ~0x38;
    P1DIR &= ~0x38; // Ingångar
    P1INP |= 0x38;  // Äkta 3-state (high impedance) utan pull-up/pull-down!

    // Vänta 150 ms medan matningarna stabiliseras
    delay_ms(150);

    // STEG 2: Släpp Blackfin ur reset ostört (utan att CC1110 belastar varken SPI eller UART)
    P0_0 = 1; // SLÄPP RESET! Blackfin bootar från U3 Flash ostört!

    // Vänta 750 ms medan Blackfin kör bootrom, laddar firmware och stabiliserar I/O
    delay_ms(750);

    // Aktivera nu Bus Switch mot Blackfin
    P1_1 = 1;

    // NU NÄR BLACKFIN HAR BOOTAT: Initiera UART1 RX med ren nollställd buffert
    URX1IE = 0;
    URX1IF = 0;
    g_uart_rx_count = 0;
    uart1_init_57600();
    delay_ms(10);

    // STEG 3: Skicka UART-kameratriggers på TX (P0_4) vid 57600 baud
    uart1_tx_str("\r\nSNAP\r\n");
    uart1_tx_str("\x02\x01\x00\x03");
    delay_ms(200); // Ge Blackfin tid att svara på UART!

    // Om vi inte fick något svar vid 57600 baud, testa 115200 baud!
    if (g_uart_rx_count == 0) {
        uart1_init_115200();
        delay_ms(10);
        uart1_tx_str("\r\nSNAP\r\n");
        uart1_tx_str("\x02\x01\x00\x03");
        delay_ms(200);
    }

    // Stäng av UART RX interrupt innan SPI-läsning
    URX1IE = 0;

    // STEG 4: Initiera CC1110 som SPI Master i Mode 3 vid 354 kHz
    spi0_master_init();
    P1_1 = 1;
    P1_2 = 1;
    delay_ms(10);

    // STEG 5: Vänta 500 ms medan kamerasensorn exponerar och JPEG komprimeras i SDRAM
    delay_ms(500);

    // STEG 6: Kontinuerlig burst-läsning av bildströmmen i Mode 3!
    P1_2 = 0; // Håll /SPISS låg oavbrutet
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

    spi0_master_txrx(0x03); // Read Stream Command
    for (i = 0; i < 2048; i++) {
        g_spi_rx_buf[i] = spi0_master_txrx(0x00);
    }

    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_2 = 1; // Deassert /SPISS

    // Sök efter JPEG SOI (0xFF 0xD8)
    for (i = 0; i < 2047; i++) {
        if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
            jpeg_start = (int16_t)i;
            break;
        }
    }

    // Om vi inte hittade JPEG på första läsningen med 0x03, prova en direktläsning utan 0x03!
    if (jpeg_start < 0) {
        P1_2 = 0;
        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
        for (i = 0; i < 2048; i++) {
            uint8_t b = spi0_master_txrx(0x00);
            if (b != 0x00) {
                g_spi_rx_buf[i] = b;
            }
        }
        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
        P1_2 = 1;

        for (i = 0; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
                jpeg_start = (int16_t)i;
                break;
            }
        }
    }

    // Om vi mottog UART-bytes från Blackfin och SPI fortfarande är tomt,
    // bädda in UART-svaret i början så vi kan läsa av Blackfins boot-logg/diagnostik!
    if (jpeg_start < 0 && g_uart_rx_count > 0) {
        g_spi_rx_buf[0] = 'U';
        g_spi_rx_buf[1] = 'A';
        g_spi_rx_buf[2] = 'R';
        g_spi_rx_buf[3] = 'T';
        g_spi_rx_buf[4] = ':';
        g_spi_rx_buf[5] = g_uart_rx_count;
        for (i = 0; i < g_uart_rx_count && i < 58; i++) {
            g_spi_rx_buf[6 + i] = g_uart_rx_buf[i];
        }
    }

    if (jpeg_start >= 0) {
        for (i = (uint16_t)jpeg_start + 2; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD9) {
                total_len = (i + 2) - (uint16_t)jpeg_start;
                break;
            }
        }
        if (total_len == 0) total_len = 2048 - (uint16_t)jpeg_start;
    }

    *out_len = total_len;

    // Återställ SPI-pinnar och behåll Port 1 i Pull-UP läge (dra ALDRIG Port 1 till pull-down, det nollar PIR P1_7!)
    P1SEL &= ~0x38;
    P1DIR &= ~0x38;
    P2INP &= ~0x40; // Port 1 Pull-UP

    // Stäng OMEDELBART av spänningarna till Blackfin och kamerasensorn (RÖDA DIODEN SLÄCKS DIREKT!)
    P0_0 = 0; // Blackfin i reset
    P0_2 = 0; // 1.2V Core rail OFF
    P0_7 = 0; // 3.27V Aux rail OFF (Röda dioden släcks!)
    P1_0 = 0; // 3.3V switched rail OFF
    P1_1 = 0; // Bus switch OFF
    P1_2 = 1; // SS HIGH
    P0 &= ~0xCF; // Släck alla eventuella LED-pinnar (P0_1, P0_3, P0_6)
    P2_0 = 0;

    // Strömma sedan JPEG till Gateway och Home Assistant över RF i lugn och ro
    if (jpeg_start >= 0 && total_len > 0) {
        stream_buffer_rf(&g_spi_rx_buf[jpeg_start], total_len, p_seq);
    } else {
        // Strömma den faktiska SPI-bufferten (2048 bytes) så vi ser resultatet direkt
        stream_buffer_rf(&g_spi_rx_buf[0], 2048, p_seq);
    }

    return jpeg_start;
}

static void stream_buffer_rf(const uint8_t *buf, uint16_t len, uint16_t *p_seq) {
    uint16_t offset = 0;
    uint16_t chunk_idx = 0;

    clock_xosc();
    radio_init();
    delay_ms(5);

    while (offset < len) {
        uint8_t chunk_len = (len - offset > 8) ? 8 : (uint8_t)(len - offset);
        bool is_first = (chunk_idx == 0);
        bool is_last = (offset + chunk_len >= len);

        transmit_image_chunk((*p_seq)++, chunk_idx++, chunk_len, is_first, is_last, &buf[offset]);
        offset += chunk_len;

        delay_ms(15);
    }
}

void main(void) {
    uint16_t seq = 25000;
    uint8_t p1_val;
    bool motion;
    bool prev_motion = false;
    uint16_t img_len = 0;

    setup_gpio();
    clock_xosc();
    radio_init();
    delay_ms(50);

    // Initial announce packet
    transmit(seq++, false, false, true, 0, 0, P1, 0);
    delay_ms(100);

    // Initial POST capture on boot: red LED illuminates for ~1.5s then turns off!
    capture_blackfin_jpeg(&img_len, &seq);
    delay_ms(500);

    // Arm motion detection (P1_7 active LOW)
    p1_val = P1;
    prev_motion = ((p1_val & 0x80) == 0);

    while (1) {
        p1_val = P1;
        motion = ((p1_val & 0x80) == 0); // Aktiv LÅG på P1_7

        if (motion && !prev_motion) {
            // PIR-rörelse detekterad! Skicka larmpaket och ta bild!
            transmit(seq++, motion, false, false, 0xEE, 0, p1_val, 0);
            delay_ms(50);
            capture_blackfin_jpeg(&img_len, &seq);
            delay_ms(2000); // PIR sensor vila och stabilisering

            // Läs av färsk nivå efter vila så vi inte loopar
            p1_val = P1;
            prev_motion = ((p1_val & 0x80) == 0);
        } else {
            // Normal telemetri till Home Assistant var 1 sekund
            transmit(seq++, motion, false, false, 0, 0, p1_val, 0);
            prev_motion = motion;
            delay_ms(1000);
        }
    }
}
