#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Zigbee network map, devices and operations through Zigbee2MQTT on the gateway's MQTT broker.
 * Call before gw_mqtt_start(): registers the MQTT listener and /api/zigbee/net. */
void zigbee_net_init(void);

#ifdef __cplusplus
}
#endif
