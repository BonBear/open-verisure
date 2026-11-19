/*
 * AES-128 coprocessor of the CC1110, shared by the door sensor and the camera firmware.
 * Included by VerisureDoorSensor/main.c and MD_CAM/firmware/main.c (each builds one
 * translation unit, so this file is not compiled on its own). Needs <cc1110.h> and <stdint.h>.
 */

/**
 * @brief Load a 128-bit key into the CC1110 AES hardware coprocessor.
 * @param key Pointer to 16-byte key in code memory (__code).
 */
void aes_load_key(const uint8_t __code *key) {
    uint16_t timeout = 50000;
    // Set command to Load Key: CMD = 10b (bit 2:1), Start: ST = 1 (bit 0)
    ENCCS = (0x02 << 1) | 0x01;
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = key[i];
    }
    // Wait until hardware coprocessor asserts RDY (bit 3)
    while (!(ENCCS & 0x08) && --timeout);
}

/* After an encrypt or decrypt ENCCS.RDY did not come up before ENCDO is read, so waiting for it
 * with a 50000 timeout cost 44 ms per block (measured on the camera with Timer 1; a picture chunk
 * took 52 ms instead of ~8 ms). Wait at most 120 short loop passes (~0.1 ms) instead, far more
 * than one AES block needs; the key load still waits for RDY (it does come up there). */
static void aes_wait(void) {
    uint8_t d;
    for (d = 0; d < 120 && !(ENCCS & 0x08); d++)
        ;
}

/**
 * @brief Encrypt a 16-byte block using hardware AES-128-ECB.
 * @param in Pointer to 16-byte plaintext input buffer.
 * @param out Pointer to 16-byte ciphertext output buffer.
 */
void aes_encrypt_block(const uint8_t *in, uint8_t *out) {
    // Set MODE = 100b (ECB -> bit 6), CMD = 00b (Encrypt -> bit 2:1), ST = 1 (bit 0)
    ENCCS = 0x40 | 0x01;
    for (uint8_t i = 0; i < 16; i++) {
        ENCDI = in[i];
    }
    aes_wait();
    // Read 16-byte encrypted ciphertext from hardware output register
    for (uint8_t i = 0; i < 16; i++) {
        out[i] = ENCDO;
    }
}
