# Hardwarové testy P4 gateway

Automatizace: [hw_test.py](hw_test.py). Testy běží proti skutečné desce v LAN a jsou destruktivní:
přepínají role, flashují C6 a zapisují slot gateway.

```sh
# výpis testů
python3 tests/hw_test.py --list
# celý běh (TC09 potřebuje aioesphomeapi, proto Python z ESPHome venv)
KEY=$(sed -n 's/^api_encryption_key: "\(.*\)"/\1/p' firmware/esphome/secrets.yaml)
../esphome/ble_proxy/.venv/bin/python tests/hw_test.py --host <ip> --esphome-key "$KEY" --report tests/results/<datum>/hw_test.json
# jen vybrané
python3 tests/hw_test.py --host <ip> --only TC04 TC05
```

## Předpoklady

- Deska flashnutá `scripts/flash_p4.sh` (čisté NVS), propojky JP1 13→20, 15→22, 17→24, Ethernet s DHCP.
- `dist/` z `scripts/build_all.sh` (TC10, TC12 nahrávají `dist/p4gw.bin` a `dist/c6fw.bin`).
- Bez hesla v UI (TC11 si heslo nastaví a zase odebere).
- TC01–TC03 předpokládají první start; další testy navazují v pořadí (TC06 a TC08 používají stav z TC04/TC05).
- TC04 v síti s jiným primárním border routerem: SRP a NAT64 této brány jsou neaktivní, test to toleruje.
- **Pozor:** testy přepínají role. Border router se díky uloženému datasetu vrací do své sítě, ale během
  testu není dostupný; neběžte je na bráně, na které právě závisí provoz domácnosti.

## Testovací případy

| ID | Co ověřuje | Kroky | Očekávaný výsledek |
|---|---|---|---|
| TC01 | První start | stav po USB flashi | role `none`, údržba žádá zálohu C6, Ethernet IPv4 + IPv6 link-local, v c6fw jsou 4 balíky |
| TC02 | Pojistka zálohy C6 | `POST /api/role thread_br` bez zálohy | 409 `c6_backup_required`, C6 nedotčen |
| TC03 | Záloha C6 přes JP1 | `GET /api/c6/backup` (asi 210 s) | celý flash C6, 0xE9 na 0x0, tabulka oddílů na 0x8000, uloženo do `backups/c6_backup_<aplikace>_<čas>.bin`, C6 označen jako zálohovaný |
| TC04 | Thread border router | přepnutí na `thread_br` | C6 = `ot_rcp`, role leader/router do 90 s, border routing běží, SRP/NAT64/Border Agent do 60 s (startují ~20 s po leaderovi), OTBR REST `/node` a dataset na portu 8081, webové CLI odpovídá |
| TC05 | Zigbee koordinátor | přepnutí na `zigbee_coordinator`, reset C6, TCP klient na 6638 | C6 = `zigbee_ncp`, NCP po resetu vysílá (UART funguje), bez chyb UART, TCP klient se zaregistruje a odpojení se pozná |
| TC06 | Zachování Zigbee sítě | koordinátor → Thread BR → koordinátor | při odchodu se uloží snapshot `zigbee_ncp`, Thread síť je beze změny, při návratu `restored 0x…` |
| TC07 | Zigbee router | přepnutí na `zigbee_router` | C6 = `zigbee_router`, stavové řádky `@ZR` přichází do 40 s, příkazy se přijímají, snapshot koordinátora zůstává |
| TC08 | Thread router | přepnutí na `thread_router`, vložení datasetu z TC04 | bez datasetu čeká, po vložení se připojí (child/router/leader) |
| TC09 | ESPHome a návrat | přepnutí na `esphome`, BLE sken 10 s, „Return to gateway“ s rolí Thread BR | port 80 zmizí, API 6053 běží, BLE advertisementy > 0, gateway se vrátí do `thread_br`, C6 zpět na `ot_rcp` |
| TC10 | OTA gateway | upload `dist/p4gw.bin` | zápis do neaktivního slotu, start v `pending_verify`, po 60 s `valid`; gateway image do slotu ESPHome odmítnut |
| TC11 | Heslo | nastavení, přístup bez/se/se špatným heslem, odebrání | 401 / 200 / 401, po odebrání opět 200 |
| TC12 | Neplatné uploady | nesmysl do c6fw a gateway, pak platný c6fw | 400, uložené image nepoškozené, 4 balíky zpět |
| TC13 | Vynucený reflash C6 | `POST /api/c6/reflash` | po restartu běží stejná role, v logu `C6 now runs` |
| TC14 | Zvuková služba | zapnout mikrofon a rozpoznávání | zachytávání bez chyb, model načten, inference pod 150 ms, CPU jádra 1 pod 40 %, klip WAV 2 s |
| TC15 | Rozpoznávání zvuků (`--play`) | `say` a ESC-50 štěkot z reproduktoru počítače u desky | Speech ≥ 0,6, Dog nebo Bark ≥ 0,5 |
| TC17 | Konfigurace událostí | neplatné ID / třída / duplicita, nová událost a výjimka, restart, předvolba | 400 pro chyby, vše přežije restart, předvolba použita, původní konfigurace obnovena |
| TC18 | Událost z přehraného zvuku (`--play`) | kočka z ESC-50, pak s ignorovanými třídami kočky | událost `cat` vznikne, s ignorovanými třídami ne |
| TC16 | Oddělené Thread sítě rolí | router se připojí do testovací sítě, pak zpět na BR | border router zůstane ve své síti |

## UI testy (prohlížeč)

Automat: [ui/ui_test.mjs](ui/ui_test.mjs), Chrome ovládaný přes `playwright-core` (nainstalovaný
Google Chrome, žádný stažený prohlížeč). Běží proti desce přes obyčejné HTTP, tedy ve stejném
nezabezpečeném kontextu jako uživatel, kde některá API prohlížeče chybí (např. schránka).

```sh
npm --prefix tests/ui ci
node tests/ui/ui_test.mjs --host <ip> [--password <heslo>]
# změna UI před flashem nebo kontrola, že test na staré stránce selže:
node tests/ui/ui_test.mjs --host <ip> --html firmware/gateway/web/index.html
```

| ID | Co ověřuje | Očekávaný výsledek |
|---|---|---|
| UI01 | všechny stránky levého menu (sekce i podsekce) | stránka se otevře (adresa `#sekce/podsekce`), žádná chybová hláška ani nezachycená výjimka JS |
| UI02 | Sound → „HA automation“ u každé události | otevře se dialog se šablonou, entity `event.`/`binary_sensor.<host>_snd_<id>`, žádná chyba |
| UI03 | editor události | otevře se s daty, „Cancel“ nic nezmění |
| UI04 | hledání ve stromu zvuků | „aircraft“ zobrazí jen odpovídající zvuky |
| UI05 | Thread → „Copy TLVs“ (role Thread) | dialog s hex datasetem, žádná chyba |
| UI06 | jazyk CZ, všechny záložky, pak EN | záložky a texty česky (`Přehled`), jména zvuků zůstanou anglicky, události mají český název (i v „Právě slyším“), volba přežije reload, EN vrátí původní texty |
| UI08 | navigace a vysvětlivky | podsekce Thread v menu, ikony mají SVG, odkaz `#system/diag` po reloadu, starý `#tools` vede na Systém › Diagnostika, ⓘ u Teploty ukáže vysvětlení, na mobilu výsuvné menu (otevře se ☰, zavře výběrem) |
| UI07 | vzhled světlý / tmavý / systém | `data-theme` a barva pozadí se mění, volba přežije reload, „systém“ sleduje nastavení OS |

## Ruční testy (mimo automat)

| ID | Co | Jak |
|---|---|---|
| M01 | Home Assistant OTBR | integrace OTBR `http://<ip>:8081`, síť jako preferovaná, Matter zařízení commissioning |
| M02 | Zigbee2MQTT | `serial: {port: tcp://<ip>:6638, adapter: zboss}`, spárovat zařízení, přepnout roli tam a zpět, zařízení dál funguje |
| M03 | Zigbee router v síti | permit join v Z2M, `steer`, router se objeví v mapě sítě |
| M04 | MQTT discovery | broker v záložce Home Assistant, v HA se objeví zařízení s výběrem role |
| M05 | Výpadek napájení | odpojit napájení v každé roli, po startu stejná role a síť |
| M06 | Výpadek Ethernetu | odpojit kabel na 1 min, Thread BR a TCP most se obnoví |
| M07 | Stabilita | 7 dní v roli Thread BR bez restartu (`uptime`, `reset_reason`) |
| M08 | Obnova továrního C6 | tovární aplikace je v `backups/c6_factory_app_network_adapter.bin` (vyextrahovaná ze zálohy P4); zapsat na 0x10000 spolu s bootloaderem a tabulkou z buildu esp_hosted, např. přes USB-UART na JP1 20/22, IO9 (pin 24) na GND |
