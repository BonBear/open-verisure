/**
 * @file esp_mac.h (host test stub)
 * @brief The factory MAC read used as the portal password salt; tests set it per "chip".
 */

#pragma once

#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0

esp_err_t esp_efuse_mac_get_default(uint8_t* mac);

/** Host tests: the MAC the next esp_efuse_mac_get_default() returns. */
void hostSetFactoryMac(const uint8_t mac[6]);
