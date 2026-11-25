/**
 * @file hal_msp430.h
 * @brief MSP430F149 Rev S Hardware Abstraction Layer for OpenVerisure SIREN.
 */

#ifndef SIREN_HAL_MSP430_H
#define SIREN_HAL_MSP430_H

#include <stdint.h>

extern volatile uint16_t g_ticks;
extern volatile uint8_t g_tamper_irq;
extern volatile uint8_t g_wor_irq;
extern uint8_t g_xtal_fault;

void hal_init(void);
void hal_sleep_lpm3(uint8_t expected_sw1);

int hal_spi_cs_low(void);
void hal_spi_cs_high(void);
uint8_t hal_spi_xfer(uint8_t b);
uint8_t hal_gdo0_read(void);
uint8_t hal_spi_so_read(void);

uint16_t hal_read_batt_mv(void);
uint16_t hal_read_th1_adc(void);

void hal_delay_ms(uint16_t ms);
void hal_delay_us(uint16_t us);
void hal_led_set(uint8_t on);

uint8_t hal_read_sw1(void);
void hal_sw1_arm_irq(uint8_t current_level);
void hal_wor_arm_irq(uint8_t enable);

void hal_wdt_hold(void);
void hal_wdt_arm_active(void);
void hal_wdt_feed(void);

uint8_t hal_reset_cause(void);
uint8_t hal_xtal_fault(void);

#endif

