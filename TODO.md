# TODO

## Hotovo (bez hardware)
- [x] Gateway firmware P4 (role Thread BR/router, Zigbee koordinátor/router), build OK
- [x] C6 flasher z P4 přes JP1, balíky c6fw, snapshoty Zigbee NVRAM, záloha C6 z UI
- [x] Web UI (přehled, role, Thread, Zigbee, HA průvodce, nástroje, systém), test proti mock API
- [x] MQTT s HA discovery (role select, restart, diagnostika)
- [x] ESPHome BLE proxy image pro slot `esphome`, návrat do brány
- [x] Vlastní Zigbee router firmware pro C6, build OK
- [x] build_all / flash / backup skripty, pinned závislosti

## Čeká
- [ ] Build `zigbee_ncp` (esp-coordinator) a `esp_hosted` (esp-hosted-mcu) a kompletní `dist/`
- [ ] Záloha flash P4 (CH340 nestabilní nad 115200 Bd) a C6
- [ ] Propojky JP1, první flash, test všech rolí na desce
- [ ] Akceptační testy ze zadani.md (HA OTBR, Matter, NAT64, SRP, 7 dní stabilita)
- [ ] Zigbee2MQTT přes TCP s ZBOSS adaptérem, přepnutí koordinátor -> Thread -> koordinátor bez ztráty sítě
- [ ] Ověřit HA cestu k "active dataset TLVs" v aktuální verzi HA (text v UI)
