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
- [x] api.json ověřeno podle kódu: odpověď /api/thread/topology (pole routerů, dětí, vendor/model/sw, časové limity), úrovně Telegramu (0 vypnuto, 1 kritické, 2 varování+, 3 vše, tiché hodiny jen kritické)
- [x] Zigbee router rádia A znovu v síti Zigbee2MQTT (PAN 0x1a62, kanál 11; párování otevřeno přes `/api/zigbee/net/request`, 2026-11-03); TC23 a TC26 prošly (TC26: text zprávy o záloze opraven)
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
- [x] UI testy UI06/UI07 přepsány na přepínače `#seg-lang`/`#seg-theme` (2026-11-03), UI25 prošel
- [x] Snímky dokumentace přefoceny na v1.10 (fáze thread a zigbee, nové stránky Toky dat, Wi-Fi, API, průvodce, tooltip MQTT), 2026-11-03
- [x] Snímky fáze `router` přefoceny na v1.10
- [ ] Dialogy 2FA a párování Telegramu přefotit s přihlášeným uživatelem (API token je spustit nemůže)
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
- [x] F2.1 ESPHome proxy 1.1.0 na rádiu B (C5): HA vidí 5 slotů (bluetooth connection allocations), diagnostika v HA (Wi-Fi −36 dBm, 35,9 °C, volná RAM 90 kB, verze, IP, síť, restart). Záznam ESPHome v HA byl „Door Sensor“ (stejná MAC XIAO), přejmenován na „P4 Gateway BLE proxy“. Volné sloty ESPHome neexponuje jako entitu
- [x] F2.2 Sloty rádií: Wi-Fi uplink jako ota_4 na C6 i C5, ESPHome jako ota_5 na C5 (`slots_c5.csv`, image pro 8 MB); přepnutí 10–28 s bez balíku; instalace layoutu 95 s (C6) / 150 s (C5); ESPHome na C6 dál celým flashem
- [ ] Sledovat: rádio B jednou po přepnutí wifi_uplink → esphome „bootloader not responding“ (2026-11-03, hned po instalaci nového layoutu); úplný restart pomohl, 4 další přepnutí v pořádku
- [ ] TC13 (vynucené přeflashování) ověřit s balíkem z vydání: lokálně nahraný balík brána po flashi zahodí a stáhne ten z vydání
- [x] F2.3 Testy: přepínání rádia A (Thread ↔ Wi-Fi) a B (ESPHome ↔ Wi-Fi, 4×) ze slotů, TC04, TC23 až po krok připojení routeru (známý problém: router bez sítě); HA vidí 5 slotů proxy

**F3 Web UI**
- [x] F3.1 Tooltipy uzlů diagramu na přehledu (LAN, P4, rádia A/B, jejich sítě)
- [x] F3.2 Provoz v tooltipech hlavičky (od startu a aktuální rychlost po službách, `/api/net/traffic` každých 10 s; firmware už počítadla měl)
- [x] F3.3 Rádia › Role: tabulka (výchozí), seznam, dlaždice, volba v prohlížeči; UI27
- [x] F3.4 Stránka Toky dat (zdroje → služby brány → odběratelé, čáry podle stavu, tooltipy s provozem, tabulka toků s odkazy na nastavení), UI28
- [x] F3.5 UI testy UI01–UI28 na bráně prošly (UI06/UI07 opraveny na přepínače v postranním panelu); snímky dokumentace zůstávají v bodu „Přefotit snímky“

**F4 Konfigurační průvodce**
- [x] F4.1 Kroky: úvod, účet, LAN, role, Wi-Fi, Thread, Zigbee, ESPHome, HA, MQTT, oznámení, zvuk, aktualizace, hotovo (nepoužité se vynechají); krok přežije restart
- [x] F4.2 Vyskakovací panel vpravo dole nad skutečnými stránkami (žádné duplicitní formuláře), sám při bráně bez účtu, ručně ze Systém › Informace a z upozornění „žádný účet“
- [x] F4.3 UI29: průchod všemi kroky, obnovení po reloadu, dokončení, samootevření bez účtu, zavření

**F5 API dokumentace**
- [x] F5.1 `api.json` (103 endpointů, cs/en) → `docs/api.md` a sekce HTTP API na GitHub Pages (`tools/docs/api_md.py`, `build_docs.py`)
- [x] F5.2 `GET /api/help` (veřejné, 32 kB gzip) a Systém › API: jak začít, příklady curl/Python/HA s adresou, tokenem a rolemi této brány, Vyzkoušet pro GET; UI30
- [x] F5.3 `tests/unit/api_spec_test.py` v CI (endpointy, metody, oprávnění z `AUTH_MAP`), kontrola aktuálnosti `docs/api.md`

**F6 Zvuk**
- [x] F6.1 Audio ring 10 s ponechán: čte ho inference (zpoždění ≤ 1 s + okno 0,96 s), učení (0,975 s) a `/api/sound/clip` (≤ 9 s); 20 s by přineslo jen delší ruční klip, žádné funkci nepomůže (+320 kB PSRAM). Smysl by dávalo až s nahráváním zvuku k události (např. příloha v Telegramu)

**F7 Uzavření**
- [x] F7.1 Regrese 2026-11-03 ([report](tests/results/2026-11-03/report.md)): vše prošlo kromě očekávaných TC01–03 (čistá deska) a známého TC26 (router bez sítě); opraveny testy TC09, TC30, TC39, UI22. Soak jen na pokyn
- [x] F7.2 Dokumentace a CHANGELOG aktualizované (api.md, airtag.md, c6-slots, installation, home-assistant, GitHub Pages)
- [x] F7.3 Vydání v1.10 a v1.10.1 (oprava nabídky balíku C6) na GitHubu, CI zelené; deska na v1.10.1 z online aktualizace, balíky C6/C5 z vydání, TC13 s balíkem z vydání prošel.

**F8 Odloženo**
- [x] F8.1 AirTag přes BLE proxy: rešerše ([docs/airtag.md](docs/airtag.md)): anonymní detekce „Find My poblíž“ jde (jen přeposílání inzerátů, Bermuda/šablona v HA), identifikace vlastních AirTagů jen s klíči exportovanými z Macu (křehké, na Sequoia/Tahoe rozbité), automatické přidávání nelze; doporučen BLE tag s pevnou adresou / iBeacon
- [ ] F8.2 (volitelné) test anonymní detekce s reálným AirTagem, pokud o ni bude zájem

## I2C senzory (zadání 2026-11-03)
Sběrnice: konektor CN3 (1 GND, 2 3V3, 3 SCL, 4 SDA) nebo JP1 pin 23 SDA (GPIO7), 25 SCL (GPIO8), sdílená s kodekem ES8311 (0x18), 100 kHz; napájení 3V3 (pin 1/3), GND (pin 5). Rozhodnutí (autonomně, bez dotazu): stránka Systém › Hardware (`system/hw`, vlastní oprávnění), automaticky se přidá jen jednoznačně rozpoznaný senzor, při konfliktu adres volí uživatel; ruční přidání s libovolnou podporovanou adresou; jméno výchozí podle čipu; expozice per senzor (výchozí zapnuto), v MQTT navíc skupina `sensors`; integrace vytváří entity dynamicky.

- [x] S1 Rešerše: senzory, adresy, ID registry, konflikty, klony ([docs/sensors.md](docs/sensors.md)); kontrola konstant proti zdrojům ESPHome, Adafruit, Sensirion, Bosch; neověřené (ID VEML7700, reset MAX44009) mají záložní cestu
- [x] S2 Sdílená sběrnice v gw_core (`gw_i2c`), zvuk ji převezme; mikrofon běží (TC47, TC14 se sníženým prahem)
- [x] S3 Komponenta `sensors`: ovladače nad abstrakcí sběrnice (testovatelné na hostu), sken, identifikace s jistotou, konfigurace v NVS, úloha čtení, `/api/sensors*`, Telegram `/sensors`
- [x] S4 MQTT: skupina `sensors`, seznam rozšíření, nová discovery při změně; opraven deadlock (discovery jen v publikační úloze), TC46
- [x] S5 HA integrace 1.6.0: entity senzorů dynamicky, testy
- [x] S6 Web UI Systém › Hardware: sběrnice a zapojení, senzory (jméno, expozice, hodnoty), sken s volbou při konfliktu, ruční přidání, interval; UI test
- [x] S7 Testy: unit (ovladače se simulovanými čipy, CI), křížová kontrola BME680/BMP3 proti Adafruit, HW TC47, TC46, UI31, UI24
- [x] S8 Dokumentace (sensors.md, hardware.md s CN3 a RS485, api.json, users, home-assistant, GitHub Pages), CHANGELOG
- [ ] S9 Test se skutečnými senzory (BME280/BMP280, SHT4x, AHT20, BH1750 …): převody zatím ověřené jen simulací
- [x] S10 Integrace 1.6.0 nasazena do HA 2026-11-03 (záloha 1.5.0 v /config/p4gw_backup_1.5.0), záznam načten, 135 entit

## RS485 / Modbus (zadání 2026-11-03)
Rozhodnutí: RTU na J4 (MAX485, TX GPIO26, RX GPIO27, UART3, automatický směr) + Modbus TCP klient; šablony JSON pro čtení i zápis (number/switch/select/button jen kde to šablona povolí); vestavěné šablony ve firmwaru, uživatelské v NVS (~50 kB volných); každé zařízení jako samostatné zařízení v HA, volitelně přes integraci nebo MQTT; menu Systém › RS485. Testovací zařízení zatím není: ověření simulací (Modbus TCP slave na Macu).

- [x] M1 Rešerše a kontra-rešerše: Eastron SDM (oficiální protokoly + manuály, ESPHome, mbmd, evcc, Tasmota, sdm_modbus), systémy šablon (evcc, ESPHome, HA modbus, Tasmota, jollyjinx), ORNO/DDS238/DTSU666/EM340/ABB/PZEM; poznámky v build/research
- [x] M2 Protokol (CRC, RTU, TCP/MBAP, výjimky, typy a pořadí slov) a model šablon s validací a plánem blokových čtení; unit testy v CI
- [x] M3 Šablony Eastron SDM120, SDM230, SDM630 V2, SDM72D-M V2 a V1 (generované z jedné tabulky, tools/modbus/gen_sdm.py)
- [x] M4 Šablony ORNO OR-WE-504/514/516, Hiking DDS238-2, Chint DTSU666/DDSU666, Peacefair PZEM-016/017 po kontra-rešerši (DTS238-7: mapa nenalezena, bez šablony; nepotvrzené registry vynechány)
- [x] M5 Komponenta `modbus`: UART3 RTU, TCP klient, zařízení, polling s dělením bloků při výjimce, zápis, offline/backoff, uživatelské šablony v NVS, API, Telegram
- [x] M6 MQTT: samostatné zařízení v HA (via_device brána), příkazy pro zápis; integrace (1.6.0, stejné vydání): zařízení a entity sensor/binary_sensor/number/switch/select/button
- [x] M7 Web UI Systém › RS485: sběrnice, zařízení s hodnotami, přidání ze šablony, hledání adresy/rychlosti, editor a import/export šablon, ruční čtení/zápis registrů
- [x] M8 Testy: vlastní Modbus TCP simulátor (tools/modbus/sim_tcp.py, bez závislostí), HW TC48 (vč. MQTT), UI32, HA testy (26); dokumentace docs/modbus.md
- [ ] M9 Test se skutečným zařízením na RS485 (časování, mapy označené „komunita“: ORNO 504, 516/517)
- [x] Vydání v1.11 (2026-11-03): CI zelené, release na GitHubu, deska na v1.11 z online aktualizace, integrace 1.6.0 v HA
- [ ] M10 Ověřit zásobník úlohy modbus (6 kB) a volnou interní RAM (203 kB po přidání I2C a Modbus)
- [x] M11 Hledání Modbus TCP v síti (Systém › Hledání Modbus TCP, `/api/modbus/scan`), rozpoznání Victron GX/VE.Bus a GoodWe, TC49, UI33; šablony Victron GX a VE.Bus ověřené na MultiPlus-II GX (192.168.88.240)
- [x] M12 GoodWe GW3000-XS: nalezen 2026-11-04 9:00 z Macu přes VPN na 192.168.1.170 (modul Solar-WiFi…, MAC 34:EA:E7:xx:xx:xx), Modbus jen UDP 8899 (AA 55), unit 0x7F; TCP zavřené. Přidán přenos `udp` (GoodWe AA55) do komponenty modbus, šablona goodwe_xs, hledání v síti ho přidá jedním klikem; TC50, UI34 se simulátorem
- [ ] M13 GW3000-XS ze skutečné brány: 2026-11-04 přes `tools/modbus/udp_relay.py` na Macu (VPN) čte brána všech 17 hodnot jako zařízení d4 (expose none, host = Mac:18899, dočasné). Trvale: trasa z 192.168.88.x do 192.168.1.0/24 na routeru, pak d4 přepnout na 192.168.1.170:8899 a expose integration; porovnat s aplikací SEMS

## Modbus TCP server (zadání 2026-11-06, plán [docs/modbus.md](docs/modbus.md#modbus-tcp-server-plan-2026-11-06))
Rozhodnutí: sdílený port 502 se směrováním podle unit id + volitelně vlastní port s přemapováním unit id; registry dotazované bránou z cache, ostatní průchodem s limitem na port; zápis volbou u serveru (výchozí jen čtení) a seznam povolených IP; zdroje RS485, GoodWe UDP i Modbus TCP zařízení; strop 8 serverů, 2 klienti na server, 8 celkem; lwIP sockety 24 → 40.
- [ ] S1 Limity lwIP, task serveru, port 502 se směrováním podle unit id, průchod čtení
- [ ] S2 Cache registrů dotazovaných bloků, limit dotazů na port, vlastní porty s přemapováním unit id
- [ ] S3 Zápis u serveru, povolené IP, zdroje GoodWe UDP a Modbus TCP
- [ ] S4 UI Systém › RS485 › Modbus TCP server, API, dokumentace
- [ ] S5 Zátěžový test: 8 klientů, role a web UI dál reagují

## Čas a NTP (zadání 2026-11-05)
- [x] Systém › LAN › Čas (`/api/time`): až 3 servery SNTP, časové pásmo brány (seznam IANA nebo pravidlo POSIX), platí hned; pásmo pro celou bránu (časy v UI a logu, tiché hodiny Telegramu, noční záloha Zigbee 03:00–04:00 místního času); pásmo přesunuto z Telegramu (stejný klíč `tz`); TC51, UI35
- [ ] Volitelně servery NTP z DHCP (volba 42): lwIP při obnově zápůjčky přepíše i ručně zadané servery, proto zatím vynecháno

## XIAO nRF54LM20A jako rádio B (odloženo 2026-11-05, deska je k dispozici)
Rešerše v [docs/dual-radio.md](docs/dual-radio.md#xiao-nrf54lm20a-deferred).
- [ ] N1 Thread RCP: NCS `samples/openthread/coprocessor` na uart21 (D6/D7 = P1.08/P1.09), 1 Mbd s RTS/CTS na volných pinech nebo nižší rychlost bez řízení toku; ověřit verzi Spinel API proti hostiteli v ESP-IDF
- [ ] N2 Detekce typu rádia B: ROM ESP / Spinel (HDLC 0x7E, PROP_NCP_VERSION) / ZBOSS NCP (0xDE 0xAD) / MCUboot SMP, na více rychlostech
- [ ] N3 Aktualizace z brány: MCUboot serial recovery + vlastní klient SMP v P4 (CBOR, base64 rámce), vstup přes RESET (`BOOT_SERIAL_PIN_RESET`) nebo pin; první flash přes USB-C (SAMD11 CMSIS-DAP)
- [ ] N4 Zigbee NCP: ověřit, zda R23 add-on 1.4 umí NCP sestavit (předkompilované knihovny) a zda je kompatibilní se Z2M `zboss`; ESPHome BLE proxy na nRF nejde

## GPIO na JP1 (zadání 2026-11-05, plán [docs/gpio.md](docs/gpio.md))
Rozhodnutí: piny rádia B volné, když rádio B nemá roli; funkce vstup, výstup, PWM, čítač pulzů, UART-TCP most, druhá sběrnice I2C, 1-Wire DS18B20, WS2812; stav po restartu volbou u pinu; test s propojkou JP1 9–11 příště.
- [x] G1 Tabulka pinů a obsazení, kresba JP1; GPIO1 a GPIO45 bez zátěže na desce (sledují vnitřní pull)
- [x] G2 Vstup, výstup, PWM, čítač; API `/api/gpio`, UI Systém › GPIO (JP1), NVS, stav po restartu, Telegram `/gpio`; TC52, UI36
- [ ] Ověřit piny rádia B jako GPIO (rádio B bez role a bez C5)
- [x] G3 Zařízení „<host> GPIO“ v integraci (1.7.0) a MQTT (skupina `gpio`, příkazy set/duty/PULSE); TC53, testy integrace
- [x] Nasadit integraci 1.7.0 do HA (2026-11-05, záloha 1.6.0 v `/config/p4gw.bak`)
- [x] Seznam publikovaných entit GPIO v NVS (odebrání i po restartu); změna brokeru a skupin nepublikuje nové skupiny na starý broker; TC53
- [x] G4 UART-TCP most (UART4), I2C sběrnice 2 (senzory `bus: 2`), 1-Wire DS18B20, WS2812 (barva, jas, duha); TC54, rozšířený TC53, testy integrace
- [ ] G4 na skutečném hardwaru: DS18B20, pásek WS2812, zařízení na UART, senzor na sběrnici 2
- [ ] G5 HW testy s propojkou JP1 9–11, dokumentace
