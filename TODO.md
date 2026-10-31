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
- [x] Uživatelé, role, oprávnění per stránka a uživatel, 2FA (TOTP, záložní kódy), API tokeny, žádosti o reset hesla (etapa 1, TC34, docs/users.md)
- [x] Uživatelé etapa 2: integrace HA přes API token (config flow, reauth) a notifikace v HA (persistentní notifikace, událost) z tématu `notify`
- [ ] Integrace HA: ověřit v UI HA nové ověření tokenem a trvalá oznámení
- [ ] Telegram: ověřit se skutečným botem (párování, notifikace, příkazy s potvrzením a TOTP)
- [x] Uživatelé etapa 3: Telegram bot
- [x] Online aktualizace C6 (krok A): manifest s balíkem C6, stažení do PSRAM, HA, Telegram, CI vydání, TC36
- [x] Repozitář veřejný, vydání v1.5 a v1.6 na GitHubu, výchozí URL manifestu GitHub Releases
- [x] Návrat na starší vydání (jen kompatibilní s layoutem desky): releases.json, UI, HA, Telegram, TC37; ověřen rollback v1.6-dirty → v1.6 → zpět a balík C6 z v1.6
- [ ] Ověřit /releases a /install v… se skutečným Telegram botem a update.install s verzí v UI HA
- [x] Dvě rádia F0 (část): externí rádio XIAO ESP32-C5 místo C6; buildy ot_rcp, zigbee_ncp, zigbee_router, scanner pro esp32c5 (`scripts/build_c6.sh all esp32c5`); záloha továrního flash XIAO; skener na C5 ověřen (Wi-Fi i 802.15.4)
- [ ] Dvě rádia F0: zapojení XIAO C5 na JP1, běh ot_rcp a zigbee_ncp na C5
- [x] Dvě rádia F1: kontext rádia v c6_flasher/role_thread/role_zigbee/netscan, klíče NVS per rádio; regrese TC04–08, 13, 19, 20, 23, 26, 33 prošla
- [x] Dvě rádia F2 (jádro): test zapojení rádia B přes ROM bootloader (/api/radios/b/probe), ověřeno s XIAO C5
- [ ] Dvě rádia F2: test při startu, UI, chybové stavy na HW (TC38, až na konci)
- [ ] Dvě rádia F3: role per rádio (TC39 Thread + Zigbee současně)
- [ ] Dvě rádia F4: firmwary rádií jen v PSRAM (C6 i C5 z vydání), mapa slotů v NVS pro přepnutí role offline, balík c5fw.bin (packer hotový), c5fw v manifestu a CI, flash per rádio
- [ ] Dvě rádia F5: nastavení sítě per rádio, záloha a obnova per rádio (TC40)
- [ ] Dvě rádia F6: ESPHome BLE proxy jako role C6 (Wi-Fi + BLE), zrušení ESPHome role na P4
- [ ] Dvě rádia F7–F8: UI, REST, MQTT, HA, Telegram, dokumentace, soak
- [x] Krok B v kódu: layout v3 (c6fw 64 kB, model 6 MB), balík C6 v noinit PSRAM, stažení při potřebě, migrační skript
- [x] Krok B na desce: migrace na layout v3 (2026-10-30), stažení balíku při startu, reinstalace layoutu z PSRAM, přepnutí rolí bez stahování, TC04/05/12/35/36; vydání v1.6
- [x] Integrace 1.2.0 v HA (entita Firmware rádia C6, přihlášení tokenem) (token globálně, chat a notifikace per uživatel, úroveň per téma, tiché hodiny, příkazy /status /lan /thread /zigbee /noise /events /restart /identify /update /role /sound /mute s potvrzením a TOTP)
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
- [x] Integrace P4 Gateway nasazena do HA (192.168.88.201), brána přidána ze zeroconf, 65 entit, trvalé push spojení
- [ ] Integrace P4 Gateway: ověřit v HA UI událost zvuku a průběh aktualizace; vypnout MQTT discovery a přegenerovat ID entit s příponou `_2` (kolidovala s MQTT entitami)
- [ ] Hlučnost: kalibrovat hlukoměrem nebo aplikací (Zvuk › Živě › Hlučnost); do té doby odhad ±6 dB
- [ ] Ověřit instalaci integrace přes HACS z veřejného repozitáře
- [x] Zvuk fáze 4: naučené zvuky (vzorky v oddílu `sounds`, profily embedding a tón, pozadí, kalibrace, nahrání WAV), TC28
- [ ] Zjistit, proč embedding YAMNet téhož zvuku kolísá mezi běhy o 0,01–0,07 (TC28; esp-dl jedno jádro, mel deterministický)
- [ ] M10: naučený zvuk z mikrofonu a spuštění události přehráním (reproduktor Macu byl ztlumený)
- [ ] Zvuk fáze 5: 24h test se zapnutou detekcí v roli Thread BR běží od 2026-10-29 15:03 na v1.4-21-gd14e4a5 (`tests/soak.py`, výsledky `tests/results/2026-10-29/soak_thread_br/`); pak ladění falešných poplachů podle `detections.jsonl` a stejný běh v roli Zigbee
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
- [ ] Role ESPHome na layoutu v3: návrat potřebuje balík C6 (stažení); ověřit celou cestu tam a zpět
