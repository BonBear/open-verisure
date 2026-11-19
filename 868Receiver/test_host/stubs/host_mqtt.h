/**
 * @file host_mqtt.h (host test stub)
 * @brief Recorder of the MQTT calls the image receiver makes (mqtt_manager.cpp is not built for
 *        the host tests): every published picture is copied, which also lets AddressSanitizer
 *        check that the whole (pointer, length) range is valid.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

struct HostMqtt {
    int images = 0;                 // mqttPublishCameraImage() calls
    int badArgs = 0;                // calls with a null pointer, an empty or a too long picture
    size_t maxLen = 0;              // longest picture published
    std::string lastDevice;
    std::vector<uint8_t> last;      // copy of the last picture published
};

extern HostMqtt hostMqtt;

/** Forget the recorded calls. */
void hostMqttReset();
