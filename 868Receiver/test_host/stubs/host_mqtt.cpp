/**
 * @file host_mqtt.cpp
 * @brief Host stub of the MQTT publisher used by image_receiver.cpp, see host_mqtt.h.
 */

#include "host_mqtt.h"
#include "image_receiver.h"
#include "mqtt_manager.h"

HostMqtt hostMqtt;

void hostMqttReset() { hostMqtt = HostMqtt(); }

bool mqttPublishCameraImage(const char* deviceId, const uint8_t* jpegBuf, size_t len) {
    hostMqtt.images++;
    if (!deviceId || !jpegBuf || len == 0 || len > MAX_IMAGE_SIZE) {
        hostMqtt.badArgs++;
        return false;
    }
    if (len > hostMqtt.maxLen) hostMqtt.maxLen = len;
    hostMqtt.lastDevice = deviceId;
    hostMqtt.last.assign(jpegBuf, jpegBuf + len);   // reads every byte: ASan checks the range
    if (hostMqtt.keepAll) {
        hostMqtt.all.push_back(hostMqtt.last);
        hostMqtt.allDevice.push_back(hostMqtt.lastDevice);
    }
    return true;
}
