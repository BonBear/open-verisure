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

void uart1_init_38400(void) {
    uart1_init(0x0A, 0x75); // 38400 @ 27 MHz
    URX1IF = 0;
    URX1IE = 0;             // Disable interrupt to prevent break condition zeros
}

void uart1_init_57600(void) {
    uart1_init(0x0B, 0x18); // 57600 @ 27 MHz
    URX1IF = 0;
    URX1IE = 0;
}

void uart1_init_115200(void) {
    uart1_init(0x0C, 0x18); // 115200 @ 27 MHz
    URX1IF = 0;
    URX1IE = 0;
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
    P1SEL &= ~0x06;      // P1_1 (SS Undre) & P1_2 (SS Övre) = GPIO outputs
    P1DIR |= 0x2E;       // P1_1, P1_2, P1_3, P1_5 = Outputs
    P1DIR &= ~0x10;      // P1_4 (MISO) = Input
    P1INP &= ~0x10;      // Pull-up on P1_4

    P1_3 = 0;            // SCK idle LOW i Mode 0 (Bekräftat från Kort #2 Saleae-mätning!)
    U0CSR = 0x00;        // SPI Master mode
    // Mode 0: CPOL=0, CPHA=0 (0x00), MSB-first (0x20) -> 0x20
    // BAUD_E = 13, BAUD_M = 173 -> 353.5 kHz
    U0GCR = 0x20 | (13 & 0x1F);
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
    U0CSR &= ~0x02; // Clear TX_BYTE
    U0DBUF = b;
    while (!(U0CSR & 0x02) && --timeout);
    U0CSR &= ~0x02;
    return U0DBUF;
}

uint16_t spi0_transfer_dsp16(uint16_t out_word) {
    uint8_t hi, lo;

    P1_2 = 1; // Deselect Flash (P1_2=1)!
    P1_1 = 0; // Assert Blackfin /SPISS på SS Undre (P1_1=0)!
    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }

    hi = spi0_master_txrx((uint8_t)(out_word >> 8));
    lo = spi0_master_txrx((uint8_t)(out_word & 0xFF));

    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }
    P1_1 = 1; // Deassert Blackfin /SPISS (P1_1=1)!
    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }

    return ((uint16_t)hi << 8) | lo;
}

uint16_t spi0_transfer_word16(uint16_t out_word) {
    uint8_t hi, lo;

    P1_1 = 1; 
    P1_2 = 0; // Assert SS Övre (P1_2=0)
    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }

    hi = spi0_master_txrx((uint8_t)(out_word >> 8));
    lo = spi0_master_txrx((uint8_t)(out_word & 0xFF));

    for (uint8_t d = 0; d < 8; d++) { __asm nop __endasm; }
    P1_2 = 1; // Deassert SS Övre (P1_2=1)
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

    // CC1110 SPI-pinnar hålls i 3-state
    P1DIR &= ~0x38; // P1_3..P1_5 = Ingångar
    P1INP |= 0x38;  // 3-state utan pull-motstånd

    // Styrpinnar till spänningsregulatorer sätts som utgångar och drivs LÅGA (0V):
    P0DIR |= 0x85;  // P0_0 (/RESET), P0_2 (1.2V Core), P0_7 (Aux/Q2) som utgångar
    P0 &= ~0x85;    // Alla drivs till 0.0 V
    P2DIR |= 0x01;  // P2_0 som utgång
    P2_0 = 0;

    // BMODE- och DSP-pinnar (P0_1, P0_3, P0_6) som INGÅNGAR med PULL-UP (exakt som vid verifierat JEDEC-test!):
    P0DIR &= ~0x4A;
    P0INP &= ~0x4A;
    P2INP &= ~0x20;

    P1DIR |= 0x07;  // P1_0 (3.3V), P1_1 (Bus Switch), P1_2 (SS) = Utgångar
    P1_0 = 0;       // 3.3V Switched rail OFF
    P1_1 = 0;       // Bus Switch OFF (Isolerad i viloläge!)
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

static bool read_button_pressed(void) {
    uint16_t d;
    bool pressed;

    // Temporarily switch Port 1 to pull-DOWN mode (P2INP bit 6 = 1) for ~50 us
    P2INP |= 0x40;
    for (d = 0; d < 1350; d++) {
        __asm nop __endasm;
    }

    // SW3 connects P1_6 to VBAT+. If pressed -> P1_6 is HIGH (1, 3.0V).
    // If released/open -> pull-down pulls P1_6 to LOW (0, 0.0V).
    pressed = ((P1 & 0x40) != 0);

    // Immediately restore Port 1 to pull-UP mode (P2INP bit 6 = 0)
    P2INP &= ~0x40;
    for (d = 0; d < 200; d++) {
        __asm nop __endasm;
    }

    return pressed;
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
    if (p0_pu != 0) {
        plain[12] = p0_pu;
        plain[13] = p0_pd;
        plain[14] = p1_pu;
        plain[15] = p2_packed;
    } else {
        plain[12] = P0;
        plain[13] = P1;
        plain[14] = P1;
        plain[15] = P2;
    }

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

    // STEG 1: Stromsatt Blackfin och sensorskenor med Blackfin i RESET
    // Sekvensering: VDDEXT (3.3V) MASTE slas pa FORE eller samtidigt med VDDINT (1.2V)
    // enligt ADSP-BF512 EE-274 for att undvika felaktig lasning av POR/PLL!
    P0DIR |= 0x85; // P0_0 (/RESET), P0_2 (1.2V), P0_7 (Aux) = Utgangar
    P1DIR |= 0x01; // P1_0 (3.3V switched) = Utgang
    P1DIR &= ~0x40; // P1_6 ar SW3 Tamper - HALL ALLTID SOM INGANG!
    P1INP &= ~0x40;

    // Hall P1_7 som ingang (PIR)
    P1DIR &= ~0x80;
    P1INP &= ~0x80;

    // Kritiskt for Blackfin BMODE (Boot Mode) och NMI:
    // P0_1, P0_3, P0_6 och P2_0 MASTE vara INGANGAR med PULL-UP sa att Blackfin
    // laser fabrikskonfigurationen BMODE=011 (SPI Master Boot fran U3 Flash) och NMI=1!
    P0DIR &= ~0x4A; // P0_1, P0_3, P0_6 = Ingangar
    P0INP &= ~0x4A; // Pull-up aktiv
    P2DIR &= ~0x01; // P2_0 = Ingang
    P2INP &= ~0x20; // Port 0 Pull-UP aktiv

    // Stromsatt i korrekt sekvens enligt Analog Devices EE-274:
    P0_0 = 0;      // Hall Blackfin i RESET medan spanningarna stabiliseras (strikt 700 ms precis som referenskort #2!)
    P1_0 = 1;      // 1. 3.3V switched rail (Q7 - VDDEXT, SDRAM, Sensor, Flash) FORST!
    delay_ms(250);
    P0_2 = 1;      // 2. 1.2V Core rail (Q13 - VDDINT) DAREFTER!
    delay_ms(250);
    P0_7 = 1;      // 3. 3.27V aux/sensor analog rail
    delay_ms(200); // Totalt exakt 700 ms stabilisering!

    // Kritiskt for Blackfin Boot ROM (BMODE=011 SPI Master fran U3 Flash):
    // Samtliga SPI-linjer (P1_1=SS_Undre, P1_2=SS_Ovre, P1_3=SCK, P1_4=MISO, P1_5=MOSI)
    // MASTE vara i akta 3-state (INGANGAR utan pull-motstand) sa Blackfin ostort kan
    // driva bussen och U3 Flash som SPI Master utan busskonflikt!
    P1SEL &= ~0x3E; // P1_1..P1_5 som GPIO
    P1DIR &= ~0x3E; // P1_1..P1_5 = ALLA INGANGAR (High-Z)!
    P1INP |= 0x3E;  // Akta 3-state (inga pull-motstand)!

    // 4. Initiera UART1 vid 57600 baud med RX-avbrott INNAN reset släpps så att ALLA bootmeddelanden fångas!
    g_uart_rx_count = 0;
    uart1_init_57600();
    delay_ms(20);

    // STEG 2: Slapp Blackfin ur reset (Blackfin bootar och startar kamera!)
    P0_0 = 1; // SLAPP RESET! Blackfin startar!

    // Ge Blackfin och sensorn 1200 ms för boot, I2C och exponering
    delay_ms(1200);

    // Skicka trigger-kommandon över UART
    uart1_tx_str("SNAP\r\n");
    uart1_tx_str("IMAGE\r\n");
    uart1_tx_byte(0x04);
    uart1_tx_byte(0x01);
    delay_ms(50);

    // STEG 3: Nu tar CC1110 over som SPI Master Mode 0
    spi0_master_init();
    P1DIR |= 0x2E;  // P1_1, P1_2, P1_3, P1_5 = Utgangar
    P1_1 = 1;       // SS idle HIGH
    P1_2 = 1;
    delay_ms(10);

    // STEG 4: Mjukvarusvep i SPI Mode 0 med BUS SWITCH PÅ (P1_1=1)
    // KANDIDAT 0: 16-bitars ordutläsning med obligatorisk /SPISS-toggling (Blackfin CPHA=0 framing)
    P1_1 = 1; // Bus switch AKTIV
    P1_2 = 1;

    // Skicka triggerstrobar på alla kanaler
    spi0_transfer_dsp16(0x0004);
    spi0_transfer_word16(0x0004);
    uart1_tx_str("SNAP\r\n");

    // Vänta och polla i upp till 2.5 sekunder medan DSP och sensor komprimerar bild
    for (uint8_t poll = 0; poll < 25; poll++) {
        delay_ms(100);

        // Skicka Read Stream 0x0003
        spi0_transfer_dsp16(0x0003);

        for (i = 0; i < 64; i += 2) {
            uint16_t w = spi0_transfer_dsp16(0x0000);
            g_spi_rx_buf[i]     = (uint8_t)(w >> 8);
            g_spi_rx_buf[i + 1] = (uint8_t)(w & 0xFF);
        }

        for (i = 0; i < 63; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
                jpeg_start = (int16_t)i;
                break;
            }
        }
        if (jpeg_start >= 0) break;

        // Prova även word16 (P1_2)
        spi0_transfer_word16(0x0003);
        for (i = 0; i < 64; i += 2) {
            uint16_t w = spi0_transfer_word16(0x0000);
            g_spi_rx_buf[i]     = (uint8_t)(w >> 8);
            g_spi_rx_buf[i + 1] = (uint8_t)(w & 0xFF);
        }

        for (i = 0; i < 63; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
                jpeg_start = (int16_t)i;
                break;
            }
        }
        if (jpeg_start >= 0) break;
    }

    // Läs ut hela 2048-bytesbufferten
    for (i = 0; i < 2048; i += 2) {
        uint16_t w = spi0_transfer_dsp16(0x0000);
        g_spi_rx_buf[i]     = (uint8_t)(w >> 8);
        g_spi_rx_buf[i + 1] = (uint8_t)(w & 0xFF);
    }

    for (i = 0; i < 2047; i++) {
        if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
            jpeg_start = (int16_t)i;
            break;
        }
    }

    // STEG 5: Om 16-bitars word-framing inte hittade JPEG, prova Mode 3 (CPHA=1) Burst Read!
    if (jpeg_start < 0) {
        spi0_master_config(3, 13);
        P1_1 = 1; // Bus Switch MASTE vara HOG (P1_1=1)!
        P1_2 = 0; // Assert Blackfin /SPISS (P1_2=0)
        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }

        spi0_master_txrx(0x03); // Read Stream Command
        for (i = 0; i < 2048; i++) {
            g_spi_rx_buf[i] = spi0_master_txrx(0x00);
        }
        for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
        P1_2 = 1; // Deassert Blackfin /SPISS

        for (i = 0; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD8) {
                jpeg_start = (int16_t)i;
                break;
            }
        }
    }

    // Hitta JPEG EOI (0xFF 0xD9) om SOI hittades
    if (jpeg_start >= 0) {
        for (i = (uint16_t)jpeg_start + 2; i < 2047; i++) {
            if (g_spi_rx_buf[i] == 0xFF && g_spi_rx_buf[i + 1] == 0xD9) {
                total_len = (i + 2) - (uint16_t)jpeg_start;
                break;
            }
        }
        if (total_len == 0) total_len = 2048 - (uint16_t)jpeg_start;
    } else {
        // Om UART tog emot bytes och SPI gav 0x00, bädda in UART-texten i början!
        if (g_uart_rx_count > 0) {
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
        total_len = 2048; // Strömma hela rå-bufferten så vi ser exakt vad MISO/UART gav
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
    // Slack Q2 och roda dioden LD1 i vilolage:
    P2DIR |= 0x01;
    P2_0 = 0;

    // Släck Q2 och röda dioden LD1 i viloläge (3.0 mA):
    P0DIR |= 0xCF;
    P0 &= ~0xCF;

    // Annonsera UART RX-resultat via telemetri:
    transmit((*p_seq)++, false, false, false, 0xDD, g_uart_rx_count, 
             g_uart_rx_count > 0 ? g_uart_rx_buf[0] : 0, 
             g_uart_rx_count > 1 ? g_uart_rx_buf[1] : 0);
    delay_ms(20);

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

/* ============================================================================
 * U3 Flash-dump över RF (U3 /CS = inverterad P1_1, uppmätt 2026-10-30)
 * Blackfin hålls i reset. Varje block på 256 bytes skickas som en egen "bild":
 * huvudchunk [0xA5, 'U', addr23..16, addr15..8, addr7..0, JEDEC0..2] + 32 datachunkar.
 * Mottagaren känner igen tappade paket på att längden inte blir 264 bytes.
 * Först skannas hela U3 (2048 sidor à 256 B) och en bitmapp över sidor som inte
 * är 0xFF skickas med typ 'B'; därefter skickas de första U3_MAX_PAGES sådana sidor.
 * ============================================================================ */
#define U3_DUMP_AT_BOOT 1
#define U3_SEG_LEN      256
#define U3_PAGES        2048
#define U3_MAX_PAGES    64

static void u3_select(bool on) {
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
    P1_1 = on ? 1 : 0; // P1_1 = 1 -> U3 /CS låg
    for (uint8_t d = 0; d < 20; d++) { __asm nop __endasm; }
}

static void u3_send_segment(uint8_t type, uint32_t addr, const uint8_t *jedec,
                            const uint8_t *data, uint16_t *p_seq) {
    uint8_t hdr[8];
    uint16_t i;

    hdr[0] = 0xA5;
    hdr[1] = type;
    hdr[2] = (uint8_t)(addr >> 16);
    hdr[3] = (uint8_t)(addr >> 8);
    hdr[4] = (uint8_t)addr;
    hdr[5] = jedec[0];
    hdr[6] = jedec[1];
    hdr[7] = jedec[2];
    transmit_image_chunk((*p_seq)++, 0, 8, true, false, hdr);
    delay_ms(15);
    for (i = 0; i < U3_SEG_LEN; i += 8) {
        bool is_last = (i + 8 >= U3_SEG_LEN);
        transmit_image_chunk((*p_seq)++, (uint16_t)(1 + i / 8), 8, false, is_last, &data[i]);
        delay_ms(15);
    }
    delay_ms(300); // låt gatewayen publicera blocket på MQTT
}

static void dump_u3_rf(uint16_t *p_seq) {
    uint8_t jedec[3];
    __xdata uint8_t *bitmap = &g_spi_rx_buf[0];          // 2048 bitar = 256 B
    __xdata uint8_t *page   = &g_spi_rx_buf[U3_SEG_LEN];
    uint16_t pg, i;
    uint8_t sent = 0;

    // Blackfin i reset, /SPISS inaktiv, U3 avvald
    P0DIR |= 0x05;  // P0_0 (/RESET), P0_2 (1.2V)
    P0_0 = 0;
    P1SEL &= ~0x07;
    P1DIR |= 0x07;  // P1_0 (3.3V), P1_1 (U3 /CS), P1_2 (Blackfin /SPISS)
    P1_2 = 1;
    P1_1 = 0;

    // VDDEXT före VDDINT (EE-274). 3.3V-skenan matar U3.
    P1_0 = 1;
    delay_ms(10);
    P0_2 = 1;
    delay_ms(50);

    spi0_master_init();
    P1_1 = 0;
    P1_2 = 1;
    delay_ms(5);

    u3_select(true);
    spi0_master_txrx(0x9F);
    jedec[0] = spi0_master_txrx(0x00);
    jedec[1] = spi0_master_txrx(0x00);
    jedec[2] = spi0_master_txrx(0x00);
    u3_select(false);

    // Skanna hela flashen i en kontinuerlig läsning från 0x000000
    for (i = 0; i < U3_SEG_LEN; i++) bitmap[i] = 0;
    u3_select(true);
    spi0_master_txrx(0x03);
    spi0_master_txrx(0x00);
    spi0_master_txrx(0x00);
    spi0_master_txrx(0x00);
    for (pg = 0; pg < U3_PAGES; pg++) {
        uint8_t all = 0xFF;
        for (i = 0; i < U3_SEG_LEN; i++) {
            all &= spi0_master_txrx(0x00);
        }
        if (all != 0xFF) bitmap[pg >> 3] |= (uint8_t)(1 << (pg & 7));
    }
    u3_select(false);

    u3_send_segment('B', 0, jedec, bitmap, p_seq);

    for (pg = 0; pg < U3_PAGES && sent < U3_MAX_PAGES; pg++) {
        uint32_t addr = (uint32_t)pg * U3_SEG_LEN;
        if (!(bitmap[pg >> 3] & (uint8_t)(1 << (pg & 7)))) continue;

        u3_select(true);
        spi0_master_txrx(0x9F);
        jedec[0] = spi0_master_txrx(0x00);
        jedec[1] = spi0_master_txrx(0x00);
        jedec[2] = spi0_master_txrx(0x00);
        u3_select(false);

        u3_select(true);
        spi0_master_txrx(0x03);
        spi0_master_txrx((uint8_t)(addr >> 16));
        spi0_master_txrx((uint8_t)(addr >> 8));
        spi0_master_txrx((uint8_t)addr);
        for (i = 0; i < U3_SEG_LEN; i++) {
            page[i] = spi0_master_txrx(0x00);
        }
        u3_select(false);

        u3_send_segment('U', addr, jedec, page, p_seq);
        sent++;
    }

    // Stäng av skenorna och återgå till 3.0 mA viloläge
    P1SEL &= ~0x38;
    P1DIR &= ~0x38;
    P1_1 = 0;
    P1_0 = 0;
    P0_2 = 0;
    P0SEL = 0x00;
    P0DIR |= 0xCF;
    P0 &= ~0xCF;
    P2DIR |= 0x01;
    P2_0 = 0;
}

void main(void) {
    uint16_t seq = 25000;
    uint8_t cur_p1;
    bool motion;
    bool prev_motion;
    bool motion_edge;
    bool btn_pressed;
    bool prev_btn;
    bool btn_edge;
    uint16_t img_len = 0;

    setup_gpio();
    clock_xosc();

    radio_init();
    delay_ms(50);

    // Initial announce packet
    transmit(seq++, false, false, true, 0, 0, 0, 0);
    delay_ms(100);

#if U3_DUMP_AT_BOOT
    delay_ms(1500); // ge mottagarskriptet tid att ansluta efter kallstart
    dump_u3_rf(&seq);
#else
    // STEG H: UNCLAMPED BLACKFIN BOOT (P0_0=1 FÖRST, SEDAN P1_2: 0 -> 1) + SALEAE CAPTURE + SPI/CS-SVEP!
    {
        uint16_t log_idx = 64;
        uint8_t prev_p1;

        for (uint16_t i = 0; i < 512; i++) g_spi_rx_buf[i] = 0xEE;

        T1CTL = 0x0D; // Timer 1 Free-Running (4.7407 us/tick)
        URX1IE = 0;
        P0SEL = 0x00;
        P1SEL = 0x00;
        P2SEL &= ~0x07;
        P0DIR = 0xDF;
        P0 = 0x00;
        P0INP = 0x00;
        P1DIR = 0x07; // P1_0, P1_1, P1_2 utgångar; P1_3..P1_7 ingångar
        P1 = 0x04;    // P1_2 = 1, P1_0 = 0, P1_1 = 0
        P1INP = 0x00; // 20 kOhm pull-up AKTIV på P1_3..P1_7!
        P2DIR |= 0x01;
        P2_0 = 0;
        P2INP &= ~0xE0;

        // 4s pre-wait i 3.0 mA viloläge så build_and_flash_md_cam.py avslutar och Saleae hinner starta!
        delay_ms(1500);

        // =====================================================================
        // H1: Slå på P1_0=1, P0_2=1, P0_7=1, P0_0=1 MED P1_2 = 0 (och P1_1 = 0)!
        //     Vänta ~50 ms och släpp sedan P1_2 = 1! Logga P1-flanker!
        // =====================================================================
        // Boot-krav (memory BLACKFIN_BOOT_FIX): P1_1 = 1 (bussomkopplare öppen mot U3) och
        // P1_2 = 1 (/SPISS inaktiv, annars Mode Fault i Boot ROM) INNAN reset släpps.
        // P1_3..P1_5 är ingångar utan pull så Blackfin kan driva SCK/MOSI mot U3.
        P1INP |= 0x38;
        P1_2 = 1;
        P1_1 = 1;
        P1_0 = 1;
        delay_ms(10);
        P0_2 = 1;
        delay_ms(10);
        P0_7 = 1;
        P0_4 = 1;
        delay_ms(100);
        P0_0 = 1;
        delay_ms(300); // Saleae: förvänta 0x05/0x9F/0x0B-läsningar mot U3 här (ref. kort #2)
        P1_2 = 0;

        g_spi_rx_buf[0] = P1; // P1 vid P0_0=1, P1_2=0, P1_1=0
        prev_p1 = P1;
        g_spi_rx_buf[1] = 0;  // Flankräknare H1
        P1_2 = 1;             // SLÄPP P1_2 = 1!
        for (uint16_t t = 0; t < 50000; t++) {
            uint8_t c1 = P1;
            if (c1 != prev_p1) {
                if (g_spi_rx_buf[1] < 255) g_spi_rx_buf[1]++;
                if (log_idx <= 200) {
                    uint8_t tl = T1CNTL;
                    uint8_t th = T1CNTH;
                    g_spi_rx_buf[log_idx++] = 0xA1;
                    g_spi_rx_buf[log_idx++] = th;
                    g_spi_rx_buf[log_idx++] = tl;
                    g_spi_rx_buf[log_idx++] = c1;
                }
                prev_p1 = c1;
            }
        }
        g_spi_rx_buf[2] = P1; // P1 efter H1 (förväntas 0xFD!)

        // =====================================================================
        // H2: Pulsa P1_2 = 0 (50 ms) med P1_1 = 1 (U3 aktiv!) och släpp P1_2 = 1!
        // =====================================================================
        P1_2 = 0;
        P1_1 = 1;
        delay_ms(20);
        g_spi_rx_buf[3] = P1; // P1 vid P0_0=1, P1_2=0, P1_1=1
        prev_p1 = P1;
        g_spi_rx_buf[4] = 0;  // Flankräknare H2
        P1_2 = 1;             // SLÄPP P1_2 = 1 med U3 öppen!
        for (uint16_t t = 0; t < 50000; t++) {
            uint8_t c1 = P1;
            if (c1 != prev_p1) {
                if (g_spi_rx_buf[4] < 255) g_spi_rx_buf[4]++;
                if (log_idx <= 320) {
                    uint8_t tl = T1CNTL;
                    uint8_t th = T1CNTH;
                    g_spi_rx_buf[log_idx++] = 0xA2;
                    g_spi_rx_buf[log_idx++] = th;
                    g_spi_rx_buf[log_idx++] = tl;
                    g_spi_rx_buf[log_idx++] = c1;
                }
                prev_p1 = c1;
            }
        }
        g_spi_rx_buf[5] = P1; // P1 efter H2

        // =====================================================================
        // H3: Pulsa P1_2 = 0 (50 ms) med P0_1=1, P0_3=1, P0_6=1, P2_0=1, P1_1=1
        //     och släpp P1_2 = 1!
        // =====================================================================
        P1_2 = 0;
        P0_1 = 1;
        P0_3 = 1;
        P0_6 = 1;
        P2_0 = 1;
        P1_1 = 1;
        delay_ms(20);
        g_spi_rx_buf[6] = P1;
        prev_p1 = P1;
        g_spi_rx_buf[7] = 0;  // Flankräknare H3
        P1_2 = 1;             // SLÄPP P1_2 = 1!
        for (uint16_t t = 0; t < 50000; t++) {
            uint8_t c1 = P1;
            if (c1 != prev_p1) {
                if (g_spi_rx_buf[7] < 255) g_spi_rx_buf[7]++;
                if (log_idx <= 440) {
                    uint8_t tl = T1CNTL;
                    uint8_t th = T1CNTH;
                    g_spi_rx_buf[log_idx++] = 0xA3;
                    g_spi_rx_buf[log_idx++] = th;
                    g_spi_rx_buf[log_idx++] = tl;
                    g_spi_rx_buf[log_idx++] = c1;
                }
                prev_p1 = c1;
            }
        }
        g_spi_rx_buf[8] = P1; // P1 efter H3
        g_spi_rx_buf[9] = P0; // P0 efter H3

        // =====================================================================
        // H4: MED P1 I UNCLAMPED LÄGE (0xFD) OCH P1_1 = 0 (U3 AVSTÄNGD):
        //     Testa bit-bang SPI (med 20k pull-up på MISO P1_4!) under 6 olika CS-kombinationer:
        //     K0: P1_2 = 1 (Alla höga)
        //     K1: P1_2 = 0
        //     K2: P0_1 = 0
        //     K3: P0_3 = 0
        //     K4: P0_6 = 0
        //     K5: P2_0 = 0
        // =====================================================================
        P1_1 = 0;      // Stäng av U3!
        P1DIR = 0x2F;  // P1_0..P1_3, P1_5 utgångar; P1_4 (MISO) ingång med 20k pull-up!
        P1INP = 0x00;  // 20k pull-up på P1_4!
        P1_3 = 0;      // SCK idle low

        for (uint8_t k = 0; k < 6; k++) {
            // Återställ alla CS-kandidater till HÖGA (1)
            P1_2 = 1; P0_1 = 1; P0_3 = 1; P0_6 = 1; P2_0 = 1;
            delay_ms(1);
            if (k == 1) P1_2 = 0;
            else if (k == 2) P0_1 = 0;
            else if (k == 3) P0_3 = 0;
            else if (k == 4) P0_6 = 0;
            else if (k == 5) P2_0 = 0;
            delay_ms(1);

            g_spi_rx_buf[16 + k * 4 + 0] = P1; // P1 före klockning (visar MISO bit 4!)
            {
                const uint8_t cmds[3] = {0x04, 0x00, 0x00};
                for (uint8_t c_idx = 0; c_idx < 3; c_idx++) {
                    uint8_t out_b = cmds[c_idx];
                    uint8_t in_b = 0;
                    for (uint8_t b = 0; b < 8; b++) {
                        P1_5 = (out_b & 0x80) ? 1 : 0;
                        out_b <<= 1;
                        P1_3 = 1;
                        in_b = (in_b << 1) | (P1_4 ? 1 : 0);
                        P1_3 = 0;
                    }
                    g_spi_rx_buf[16 + k * 4 + 1 + c_idx] = in_b;
                }
            }
        }
        P1_2 = 1;

        // Läs även U3 @ 0x07F000 (3B) och 0x000000 (4B) efter H1..H4!
        spi0_master_init();
        P1_1 = 0; delay_ms(2);
        P1_1 = 1; delay_ms(1);
        spi0_master_txrx(0x03);
        spi0_master_txrx(0x07);
        spi0_master_txrx(0xF0);
        spi0_master_txrx(0x00);
        g_spi_rx_buf[40] = spi0_master_txrx(0x00);
        g_spi_rx_buf[41] = spi0_master_txrx(0x00);
        g_spi_rx_buf[42] = spi0_master_txrx(0x00);
        g_spi_rx_buf[43] = spi0_master_txrx(0x00);
        P1_1 = 0; delay_ms(2);
        P1_1 = 1; delay_ms(1);
        spi0_master_txrx(0x03);
        spi0_master_txrx(0x00);
        spi0_master_txrx(0x00);
        spi0_master_txrx(0x00);
        g_spi_rx_buf[44] = spi0_master_txrx(0x00);
        g_spi_rx_buf[45] = spi0_master_txrx(0x00);
        g_spi_rx_buf[46] = spi0_master_txrx(0x00);
        g_spi_rx_buf[47] = spi0_master_txrx(0x00);
        P1_1 = 0;

        // =====================================================================
        // Stäng av alla rails och återgå till 3.0 mA viloläge!
        // =====================================================================
        P1_1 = 0;
        P1_0 = 0;
        P1SEL &= ~0x38;
        P1DIR &= ~0x38;
        P1INP = 0x00;
        P0SEL = 0x00;
        P0DIR |= 0xCF;
        P0 &= ~0xCF;
        P2DIR |= 0x01;
        P2_0 = 0;
        delay_ms(10);

        transmit(seq++, false, false, false, 0xED, g_spi_rx_buf[1], g_spi_rx_buf[4], g_spi_rx_buf[7]);
        delay_ms(100);

        stream_buffer_rf(g_spi_rx_buf, 512, &seq);
        delay_ms(200);
    }
#endif

    // Arm motion and button detection
    cur_p1 = P1;
    prev_motion = ((cur_p1 & 0x80) == 0);
    prev_btn = read_button_pressed();

    while (1) {
        // Avsök knapp var 10:e millisekund (100 Hz respons!)
        for (uint8_t tick = 0; tick < 100; tick++) {
            cur_p1 = P1;
            motion = ((cur_p1 & 0x80) == 0); // För telemetri
            btn_pressed = read_button_pressed();
            btn_edge = (btn_pressed && !prev_btn);

            if (btn_edge) {
                break;
            }
            prev_motion = motion;
            prev_btn = btn_pressed;
            delay_ms(10);
        }

        if (btn_edge) {
            // OMEDELBAR BILDTRIGGER EXKLUSIVT PÅ KNAPP SW3!
            transmit(seq++, motion, true, false, 0xEE, 0, cur_p1, 0);
            delay_ms(50);
            capture_blackfin_jpeg(&img_len, &seq);
            delay_ms(1000); // Stabilisering

            // Läs av färska nivåer efter bildtagning så vi inte loopar
            cur_p1 = P1;
            prev_motion = ((cur_p1 & 0x80) == 0);
            prev_btn = read_button_pressed();
        } else {
            // Normal telemetri till Home Assistant var 1 sekund
            transmit(seq++, motion, btn_pressed, false, 0, 0, cur_p1, 0);
            prev_motion = motion;
            prev_btn = btn_pressed;
        }
    }
}
