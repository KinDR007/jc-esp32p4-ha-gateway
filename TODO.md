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
- [ ] Ověřit automatickou noční zálohu Zigbee (02:00–03:00 UTC) v roli router; v testu TC26 ověřena jen záloha na požádání
- [x] Brána vrácena do Thread sítě `MyHome284238473` (uživatel)
- [x] Zvuk fáze 3: události (předvolby, editor, výjimky, log), MQTT event/binary_sensor/device trigger, TC17/TC18
- [x] Web UI v češtině a angličtině, přepínač vzhledu světlý / tmavý / systém (UI06–UI07)
- [x] Přestavba web UI: levé menu s podsekcemi, ikony MDI, vysvětlivky, podrobné HW informace P4/C6 (UI08)
- [x] Údaje o čipu C6 z bootloaderu (MAC, revize, flash) ověřeny
- [x] Telemetrie C6 (teplota, RAM, uptime) pro Thread RCP, Zigbee koordinátor a router, TC19
- [x] Síťové okolí: rychlý a úplný sken, doporučení kanálů (TC20, UI09)
- [ ] Ověřit pojistku startů a 3s pojistku telemetrie na skutečném selhání
- [ ] Ověřit entity a události zvuku v Home Assistant (MQTT broker vyžaduje přihlášení)
- [x] Zvuk fáze 4: naučené zvuky (vzorky v oddílu `sounds`, profily embedding a tón, pozadí, kalibrace, nahrání WAV), TC28
- [ ] M10: naučený zvuk z mikrofonu a spuštění události přehráním (reproduktor Macu byl ztlumený)
- [ ] Zvuk fáze 5: 24h test se zapnutou detekcí v roli Thread BR běží od 2026-10-29 11:24 (`tests/soak.py`, výsledky `tests/results/2026-10-29/soak_thread_br/`); pak ladění falešných poplachů podle `detections.jsonl` a stejný běh v roli Zigbee
- [ ] Ruční testy M01–M08 (HA OTBR + Matter, Zigbee2MQTT, router v síti, MQTT, výpadky, 7 dní)
- [ ] Sledovat jednorázové zablokování HTTP po záloze C6 (TC03, 1. běh, nereprodukováno)
- [x] OTA brány: občas reset spojení a restart do starého slotu. Příčina (core dump 2026-10-29): task watchdog, IDLE1 hladověl, na CPU 1 běžel model zvuku zpomalený zápisy do flash (cache vypnutá pro obě jádra). Oprava: zámek `gw_flash_lock`, model během nahrávání a flashování C6 neběží
- [ ] Sloty C6: dvakrát po sobě výpadek RCP po startu Thread při rychlém sledu přepnutí (TC23 hned po přerušeném běhu, 2026-10-29 07:14); obnova sama (vynucený reflash). V 16 dalších přepnutích nereprodukováno; log před restartem po RCP failure je teď v `/api/logs/previous` (TC24)
- [ ] Ověřit modální okno flashování i při úplném skenu okolí a přepnutí role (ověřeno jen Přeflashovat C6)
- [ ] Volitelně balík `esp_hosted_factory` z backups/c6_factory_app_network_adapter.bin
- [x] Vysílací výkon C6 (Thread, Zigbee router, Zigbee2MQTT) a Identify s ikonou v hlavičce (UI13)
- [ ] Ověřit zápis výkonu ze Zigbee2MQTT (`analog_output` / `tx_power`), MQTT broker vyžaduje přihlášení
- [x] Chyba: Zigbee router po návratu z jiné role nebyl v síti. Se sloty C6 (NVS se při přepnutí
      nemaže) se vrátil do sítě ve všech ~12 přepnutích 2026-10-29; hlídá TC23.
- [ ] Ověřit HA cestu k "active dataset TLVs" v aktuální verzi HA (text v UI)
