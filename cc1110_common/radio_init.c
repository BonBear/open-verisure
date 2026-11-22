/*
 * CC1110 RF core initialization (868.950 MHz @ 27.000 MHz crystal), shared by the door sensor
 * and the camera firmware. Included by both main.c files right after radio_cca.c; not
 * compiled on its own. Needs <cc1110.h>, PACKET_LEN, delay_ms(), and RF_CCA_ENABLE /
 * RF_CCA_ABS_THR (from radio_cca.c).
 */

/**
 * @brief Initialize the built-in CC1100 radio core for Verisure 868.950 MHz telemetry.
 *
 * Register calculation details:
 * - Reference RF Crystal: 27.000 MHz (X1)
 * - Target Frequency: 868.950 MHz
 *   FREQ = (868.950 * 2^16) / 27.000 = 2109167 = 0x202EEF (FREQ2=0x20, FREQ1=0x2E, FREQ0=0xEF)
 * - Data Rate: 38.418 kBaud
 *   MDMCFG4 = 0x8A, MDMCFG3 = 0x75: (256 + 117) * 2^10 * 27 MHz / 2^28 = 38,418 Baud
 *   (the gateway's CC1101 runs 38,383 Baud with its 26 MHz crystal, 0.09 % apart)
 * - Modulation: 2-FSK with 21.4 kHz frequency deviation (DEVIATN = 0x35)
 * - Sync Word: 0xD3 0x91 (30/32 bits match)
 * - Output Power: +10 dBm (PA_TABLE0..7 = 0xC2)
 */
void radio_init(void) {
    RFST = 0x04; // SIDLE strobe: put radio into idle state
    delay_ms(1);

    // Synchronization Word: 0xD3 0x91 (Matches gateway receiver)
    SYNC1 = 0xD3;
    SYNC0 = 0x91;

    // Packet Configuration: Fixed length + hardware 2-byte CRC
    PKTLEN   = PACKET_LEN;
    PKTCTRL1 = 0x04; // Append status bytes (RSSI, LQI/CRC)
    PKTCTRL0 = 0x04; // Fixed length mode, CRC calculation enabled

    // Intermediate Frequency (IF)
    CHANNR  = 0x00;
    FSCTRL1 = 0x06;
    FSCTRL0 = 0x00;

    // Frequency synthesis (868.950 MHz at 27.000 MHz crystal)
    FREQ2 = 0x20;
    FREQ1 = 0x2E;
    FREQ0 = 0xEF;

    // Modem configuration (38.418 kBaud, 2-FSK, 4B preamble, 30/32 sync bits)
    MDMCFG4 = 0x8A;
    MDMCFG3 = 0x75;
    MDMCFG2 = 0x03; // 2-FSK, 30/32 sync word bits
    MDMCFG1 = 0x22; // 4 bytes preamble
    MDMCFG0 = 0xF8;
    DEVIATN = 0x35; // 21.4 kHz deviation

    // State machine transitions: Autocalibrate IDLE -> TX, return to IDLE after packet
    MCSM2 = 0x07;
#if RF_CCA_ENABLE
    MCSM1 = 0x30; // CCA_MODE=11 (RSSI < threshold unless receiving), IDLE after RX/TX
#else
    MCSM1 = 0x00; // Return to IDLE after TX
#endif
    MCSM0 = 0x18; // Autocalibrate whenever transitioning from IDLE to TX

    FOCCFG = 0x16;
    BSCFG  = 0x6C;

    // Automatic Gain Control (AGC)
    AGCCTRL2 = 0x43;
#if RF_CCA_ENABLE
    AGCCTRL1 = 0x40 | (RF_CCA_ABS_THR & 0x0F);
#else
    AGCCTRL1 = 0x40;
#endif
    AGCCTRL0 = 0x91;

    // Front-End configuration
    FREND1 = 0x56;
    FREND0 = 0x10; // Select PA_TABLE0 for transmit power

    // Frequency Synthesizer Calibration registers (SWRS033 Table 61)
    FSCAL3 = 0xE9;
    FSCAL2 = 0x2A;
    FSCAL1 = 0x00;
    FSCAL0 = 0x1F; // Critical: Must remain 0x1F (synthesizer charge pump calibration)

    // Hardware Test Registers (SWRS033 Table 61)
    TEST2 = 0x81;
    TEST1 = 0x35;
    TEST0 = 0x09;

    // Power Amplifier Table: +10 dBm (0xC2) across all entries
    PA_TABLE0 = 0xC2;
    PA_TABLE1 = 0xC2;
    PA_TABLE2 = 0xC2;
    PA_TABLE3 = 0xC2;
    PA_TABLE4 = 0xC2;
    PA_TABLE5 = 0xC2;
    PA_TABLE6 = 0xC2;
    PA_TABLE7 = 0xC2;
}
