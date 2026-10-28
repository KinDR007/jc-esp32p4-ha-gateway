# TODO

## Hotovo (bez hardware)
- [x] Gateway firmware P4 (role Thread BR/router, Zigbee koordinátor/router), build OK
- [x] C6 flasher z P4 přes JP1, balíky c6fw, snapshoty Zigbee NVRAM, záloha C6 z UI
- [x] Web UI (přehled, role, Thread, Zigbee, HA průvodce, nástroje, systém), test proti mock API
- [x] MQTT s HA discovery (role select, restart, diagnostika)
- [x] ESPHome BLE proxy image pro slot `esphome`, návrat do brány
- [x] Vlastní Zigbee router firmware pro C6, build OK
- [x] build_all / flash / backup skripty, pinned závislosti

- [x] Kompletní `dist/` (všechny 4 C6 balíky), záloha P4, první flash
- [x] Automatické HW testy TC01–TC13 (tests/results/2026-10-28/report.md), 10/10 v regresi

- [x] Zvuk fáze 1–2: mikrofon ES8311, YAMNet int8 na P4 (60 ms/inferenci), záložka Sound, layout flash v2
- [x] Oddělené datasety Thread rolí (TC16), komprimované C6 balíky, potvrzení obrazu při restartu z UI

## Čeká
- [x] Brána vrácena do Thread sítě `MyHome284238473` (uživatel)
- [x] Zvuk fáze 3: události (předvolby, editor, výjimky, log), MQTT event/binary_sensor/device trigger, TC17/TC18
- [x] Web UI v češtině a angličtině, přepínač vzhledu světlý / tmavý / systém (UI06–UI07)
- [x] Přestavba web UI: levé menu s podsekcemi, ikony MDI, vysvětlivky, podrobné HW informace P4/C6 (UI08)
- [x] Údaje o čipu C6 z bootloaderu (MAC, revize, flash) ověřeny
- [x] Telemetrie C6 (teplota, RAM, uptime) pro Thread RCP, Zigbee koordinátor a router, TC19
- [ ] Ověřit pojistku startů a 3s pojistku telemetrie na skutečném selhání
- [ ] Ověřit entity a události zvuku v Home Assistant (MQTT broker vyžaduje přihlášení)
- [ ] Zvuk fáze 4: učicí režim (vzorky v oddílu `sounds`, profily embedding + tónové)
- [ ] Zvuk fáze 5: ladění falešných poplachů, 24h test se zapnutou detekcí
- [ ] Ruční testy M01–M08 (HA OTBR + Matter, Zigbee2MQTT, router v síti, MQTT, výpadky, 7 dní)
- [ ] Sledovat jednorázové zablokování HTTP po záloze C6 (TC03, 1. běh, nereprodukováno)
- [ ] Volitelně balík `esp_hosted_factory` z backups/c6_factory_app_network_adapter.bin
- [ ] Ověřit HA cestu k "active dataset TLVs" v aktuální verzi HA (text v UI)
