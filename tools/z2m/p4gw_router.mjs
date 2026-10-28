// Zigbee2MQTT (2.x) external converter for the P4 gateway in the Zigbee router role.
// Copy to <zigbee2mqtt data>/external_converters/ and restart Zigbee2MQTT; see docs/home-assistant.md.
// Exposes Identify (blinks the icon in the gateway web UI header) and the C6 transmit power,
// which the router firmware keeps in the Analog Output cluster PresentValue (dBm).
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

export default {
    zigbeeModel: ['P4GW-ZB-ROUTER'],
    model: 'P4GW-ZB-ROUTER',
    vendor: 'Guition',
    description: 'P4 gateway, ESP32-C6 Zigbee router',
    extend: [
        m.identify(),
        m.numeric({
            name: 'tx_power',
            cluster: 'genAnalogOutput',
            attribute: 'presentValue',
            valueMin: -15,
            valueMax: 20,
            valueStep: 1,
            unit: 'dBm',
            description: 'Transmit power of the C6 radio, kept across restarts',
            access: 'ALL',
            entityCategory: 'config',
        }),
    ],
};
