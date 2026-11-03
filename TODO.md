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
- [x] Dvě rádia F0: XIAO C5 na JP1 (napájení vlastním USB-C, EN open-drain), ot_rcp i zigbee_ncp na C5 běží
- [x] Dvě rádia F1: kontext rádia v c6_flasher/role_thread/role_zigbee/netscan, klíče NVS per rádio; regrese TC04–08, 13, 19, 20, 23, 26, 33 prošla
- [x] Dvě rádia F2 (jádro): test zapojení rádia B přes ROM bootloader (/api/radios/b/probe), ověřeno s XIAO C5
- [x] Dvě rádia F2: test zapojení při startu a v UI (Systém › Rádia)
- [ ] Dvě rádia TC38: chybové stavy zapojení na HW (odpojený vodič), až na konci s uživatelem
- [x] Dvě rádia F3: role per rádio, Thread na A + Zigbee koordinátor na C5 současně (TC39); XIAO musí mít vlastní napájení USB (5 V z JP1 → RTC_PWR_GLITCH)
- [x] Dvě rádia F4: firmwary rádií jen v PSRAM (4 MB buffer, po flashi zahozen), lokální soupis per rádio v NVS (přepnutí slotu offline), c5fw.bin v CI a vydání, /api/radios, /api/radios/b/flash; ověřeno: C5 naflashováno z manifestu (80 s), přepnutí slotu offline (5 s), regrese rádia A
- [ ] Watchdog při stahování OTA (2026-10-31, jednou, ve starém buildu v1.7-13 při instalaci z lokálního manifestu s c5fw); sledovat, při opakování analyzovat coredump
- [x] Dvě rádia F5: Thread nastavení per rádio (TC40), zálohy Zigbee per rádio, zapomenutí sítě; F4b PSRAM C5
- [x] Dvě rádia F6: ESPHome BLE proxy jako role rádia (C6/C5), Wi-Fi přes Improv, ESPHome na P4 zrušeno
- [ ] ESPHome na rádiu: připojení k Wi-Fi a převzetí v HA ověřit s Wi-Fi uživatele (TC09 s nastavenou Wi-Fi)
- [x] Dvě rádia F7–F8: UI (Role, Rádia, ESPHome, hlavička, přehled, menu dle rádia), HA 1.4.0, Telegram, dokumentace
- [ ] Zigbee router rádia A: znovu připojit do sítě (záloha ztracena během vývoje F3), pak TC23
- [x] Vydání v1.8.1 (dvě rádia; tag v1.8 bez vydání, CI chyba ESPHome)
- [ ] Nasazení HA integrace 1.4.0; HW testy (Wi-Fi ESPHome, TC38, router) příště; soak až na pokyn
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
- [ ] Integrace P4 Gateway: ověřit v HA UI událost zvuku a průběh aktualizace
- [x] MQTT discovery na předvolbě „Jen co integrace nemá“, 25 entit integrace přejmenováno bez přípony `_2` (2026-11-03, na `_2` nic neodkazovalo)
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
- [x] Deadlock kontroly aktualizací a MQTT (zaseknutí brány po přepnutí rádia B), oprava 0b13d95, ověřeno 4 přepnutími
- [x] Přepracované menu webového UI (menu_redesign/): sekce Rádia a ESPHome, rozpuštěné Systém › Nastavení, zálohy Zigbee per rádio; testy přes proxy jen pro čtení
- [x] Nové menu nahráno na bránu (v1.8.1-4, USB, pak 3× OTA upload z UI bez watchdogu)
- [x] Projít UI testy s přihlášením přímo na bráně (UI21–UI23 zapisují): prošly, UI22 našel ponechanou nabídku staré URL (opraveno)
- [x] Vydání v1.9 s novým menu
- [ ] Sledovat: po flashi přes USB jednou přestalo odpovídat HTTP (TCP 80 otevřený, ping OK) do ~10 min po startu, bez logu; po resetu nereprodukováno
- [ ] UI testy UI06/UI07 (volba jazyka a motivu přes `#pref-lang`/`#pref-theme` už v UI není), UI25 (formát logu) neodpovídají současnému UI (UI25 na bráně 2026-11-02 prošel)
- [ ] Přefotit snímky dokumentace v rolích Zigbee koordinátor (`PHASE=zigbee`): mají ještě staré menu
- [x] Rádio B v UI se stejnými údaji a akcemi jako rádio A (přehled, Stav a údržba: přeflashování, záloha flash, firmwary)
- [x] Úplný sken na rádiu A i B (C5: 5 GHz, šířka, standardy), oprava WPA3 hlášené jako otevřená (skener 1.1.0); TC20 prošel
- [x] Nové balíky c6fw/c5fw (skener 1.1.0) ve vydání v1.9
- [ ] Zálohu flash rádia B přes UI vyzkoušet naostro (rádio B bez role, 8 MB)
- [ ] Sledovat: `/api/status` jednou vrátil neplatný řídicí znak v JSON (kolem 1 min po startu při nahrávání balíku C5), nereprodukováno

## Wi-Fi uplink přes rádio ([docs/wifi-uplink.md](docs/wifi-uplink.md))
- [x] W0 spike: host esp-hosted 3.0.10 přes UART se sestaví, +145 kB flash, +4,6 kB statické RAM
- [x] W1 firmware rádia `wifi_uplink` (ESP-Hosted 3.0.10, UART, Wi-Fi STA+AP) pro C6 a C5, v balících s vlastním rozložením
- [x] W2 role na P4: host na pinech rádia, nastavení Wi-Fi, priorita tras, stav v /api/status
- [x] W3 role v UI, prohození A/B (`{"swap":true}`), Telegram, přehled, integrace HA
- [x] W3b otevřený přístupový bod s captive portálem (jen pár minut po startu bez LAN a bez Wi-Fi)
- [x] W4 záložní přepnutí LAN ↔ Wi-Fi, páteř Thread s hysterezí (60 s / 5 min)
- [x] W5 testy TC41–TC45 (`tests/hw_test.py`), dokumentace
- [ ] Rychlost UART nad 921600 Bd změřit na drátech JP1 (flasher jede jen 460800)
- [ ] `dependencies.lock` zapisuje cestu záplatované kopie esp_hosted absolutně (správce komponent)
- [ ] Test matice: Zigbee koordinátor přes Wi-Fi (#6), ESPHome + uplink (#10), odpojené rádio B (#13), soak přes Wi-Fi (#16) až na vyžádání
- [ ] Automatická role Wi-Fi na rádiu A u brány bez LAN od první instalace (zatím volba v UI)
- [ ] Webový server (port 80) jednou přestal odpovídat po TC44 (ping i 8081 šly); vlastní kontrola teď restartuje s coredumpem, příčinu zjistit z coredumpu, až se to zopakuje
- [ ] TC32 (statická adresa LAN) občas selže: internet zmizí ~75 s po přepnutí, místní MQTT jede; nejspíš router po uvolnění DHCP, neověřeno


## Rozšíření funkcí (zadání add_.md, 2026-11-03)
Rozhodnutí: MQTT po kategoriích, BLE proxy jen aktivní (5 slotů), layout P4 v4a (zrušit `esphome`, sloty 2 × 4 MB), model YAMNet zůstává ve flash (6 MB), Role s přepínačem list / tabulka / dlaždice, online help z popisu API v bráně + úplná dokumentace na GitHub Pages.

**F0 Příprava**
- [x] F0.1 Rozpracované změny zacommitované (707a67a); `dependencies.lock` s absolutní cestou zůstává mimo commit (viz Wi-Fi uplink)
- [x] F0.2 Rozpočet RAM/PSRAM: mapa využití, kandidáti na přesun do PSRAM s ohledem na stabilitu. Změřeno 2026-11-03: interní RAM volná 108–129 kB (minimum 75 kB, největší blok 64 kB), statická 279 kB; PSRAM volná 24 MB z 32 MB. `s_bands` zvuku zhuštěn (66 kB → 2,4 kB .bss, výstup bit po bitu shodný, TC14/TC28/TC31), statická RAM 279 → 215 kB, volná interní 168 kB (minimum 134 kB). mbedTLS v PSRAM (volná interní RAM při TLS ~190 kB místo 128 kB, minimum po startu 145 kB místo 85 kB; TC04/10/27/35). `s_kept` (16 kB, log přes restart) a OpenThread instance zůstávají interně
- [x] F0.3 Layout v4a (gw_a 0x20000 4 MB, gw_b 0x420000 4 MB, od `c6fw` beze změny, 28 % volného místa), `scripts/migrate_layout_v4.sh` (s `--rollback`), deska převedena 2026-11-03, TC10 (OTA do nového gw_b), TC14, TC27, TC37 (vydání pro v3 na v4) prošly; v1.9 nastartovala na v4
- [x] Oprava: instalace staré nabídky po změně nebo smazání URL manifestu, než ji úloha znovu zkontroluje (TC27 instaloval v1.9 z GitHubu)

**F1 MQTT a HA**
- [x] F1.1 Výběr expozice MQTT po skupinách (roles, system, update, sound, sound_events, sound_triggers) s předvolbou „Jen co integrace nemá“ (= sound_triggers; integrace má event i binary_sensor po událostech) (UI, API `discovery_groups`, NVS `mqtt_grp`, odstranění retained configů), TC46, UI24
- [x] F1.2 Aktualizace v HA i pro C5 (entita *C5 radio firmware*, integrace 1.5.0); ESPHome proxy je v balících rádií, změna jeho verze je ve shrnutí; testy `tests/ha_integration` (test_c5_update)
- [x] F1.2b Integrace 1.5.0 nasazena do HA (záloha 1.4.0 v /config/p4gw_backup_1.4.0), entita `update.…_firmware_radia_c5` ukazuje balík C5

**F2 BLE proxy a rádia**
- [ ] F2.1 ESPHome proxy 1.1.0: aktivní, `connection_slots: 5`, diagnostika (Wi-Fi signál, uptime, teplota, volná RAM a největší blok, verze, IP, síť, restart); build C6/C5 OK (RAM 41 %). Volné sloty ESPHome neexponuje (HA je vidí v diagnostice adaptéru). Zbývá: nahrát na rádio B a ověřit v HA (přeflashování smaže klíč API, nové převzetí v HA)
- [ ] F2.2 Rozbor flash rádií: co se vejde do slotu, přepíná se slotem; ESPHome (1,45–1,5 MB) celým flashem; na C5 (8 MB) zvážit ESPHome jako slot
- [ ] F2.3 Testy: TC09 rozšířený (aktivní připojení, sloty), regrese rolí

**F3 Web UI**
- [ ] F3.1 Tooltipy v přehledu HW (první karta a její položky)
- [ ] F3.2 Tooltipy štítků v hlavičce s datovým provozem (fw: počítadla ETH, Wi-Fi, Thread, MQTT v `/api/status`)
- [ ] F3.3 Rádia › Role: kompaktní výpis s přepínačem list / tabulka / dlaždice
- [ ] F3.4 Vizualizace HW flow s tooltipy a prokliky do nastavení, bez CDN
- [ ] F3.5 UI testy, snímky dokumentace

**F4 Konfigurační průvodce**
- [ ] F4.1 Návrh kroků (účet → síť → rádia a role → Thread/Zigbee/ESPHome → MQTT/HA/Telegram → zvuk → aktualizace), pokračování po restartu
- [ ] F4.2 Modální okno, první spuštění automaticky, později ručně
- [ ] F4.3 Testy: čistá deska, upgrade, přerušení

**F5 API dokumentace**
- [ ] F5.1 Úplná dokumentace API (od tokenu po příkazy) na GitHub Pages
- [ ] F5.2 Online help v UI z kompaktního popisu API v bráně, příklady z aktuálních hodnot
- [ ] F5.3 Test shody dokumentace s endpointy

**F6 Zvuk**
- [ ] F6.1 Audio ring 10 s → 20 s jen při změřeném přínosu

**F7 Uzavření**
- [ ] F7.1 Úplná regrese (soak jen na pokyn)
- [ ] F7.2 Dokumentace, CHANGELOG, vydání (push jen na vyžádání)

**F8 Odloženo**
- [ ] F8.1 AirTag přes BLE proxy: rešerše proveditelnosti, test s reálným AirTagem
