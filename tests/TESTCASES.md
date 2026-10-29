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
| TC13 | Vynucený reflash C6 | `POST /api/c6/reflash` | po restartu běží stejná role, v logu `C6 now runs`; se sloty se znovu zapíše celé rozložení |
| TC14 | Zvuková služba | zapnout mikrofon a rozpoznávání | zachytávání bez chyb, model načten, inference pod 150 ms, CPU jádra 1 pod 40 %, klip WAV 2 s |
| TC15 | Rozpoznávání zvuků (`--play`) | `say` a ESC-50 štěkot z reproduktoru počítače u desky | Speech ≥ 0,6, Dog nebo Bark ≥ 0,5 |
| TC17 | Konfigurace událostí | neplatné ID / třída / duplicita, nová událost a výjimka, restart, předvolba | 400 pro chyby, vše přežije restart, předvolba použita, původní konfigurace obnovena |
| TC18 | Událost z přehraného zvuku (`--play`) | kočka z ESC-50, pak s ignorovanými třídami kočky | událost `cat` vznikne, s ignorovanými třídami ne |
| TC20 | Síťové okolí | rychlý sken v roli Thread BR, pak úplný sken (restart, skenovací firmware C6) | rychlý: 16 kanálů, vlastní Thread síť nalezena, bez výpadku RCP; úplný: skener odpoví, 16 kanálů, Wi-Fi bez duplicit, vlastní PAN v provozu; role a Thread síť po skenu zpět |
| TC23 | Sloty C6 | přepnutí thread_br → zigbee_router → thread_br | c6fw obsahuje balíček `slots`, rozložení se znovu nezapisuje, vybere se jen slot (otadata), router se vrátí do své sítě, Thread se připojí, každé přepnutí do minuty |
| TC24 | Log před restartem | restart přes API | `/api/logs/previous` vrátí log předchozího běhu končící požadavkem na restart, aktuální log ho neobsahuje |
| TC25 | Záloha nastavení | export, chybné importy, import se změnou a obnova | export bez stavu desky a hesla jen se `secrets=1`; neplatný base64, neznámá role a cizí formát odmítnuty bez změny; import platí po restartu, klíč stavu desky v souboru ignorován (C6 se nepřeflashuje); původní nastavení obnoveno |
| TC26 | Záloha Zigbee | thread_br → zigbee_router, záloha na požádání, zpět | v roli Thread odmítnuto (409), interval jen 0/1/7; záloha zvýší generaci a uloží čas, nic se neflashuje, router se vrátí do sítě; odchod z role dál ukládá snapshot |
| TC27 | Online aktualizace: kontroly | neplatná URL, kontrola a instalace bez nabídky, chybějící manifest, ne-manifest, HTTPS | odmítnutí 400/409, čitelné chyby, HTTPS projde TLS (404 ze serveru), instalace bez nabídky odmítnuta |
| TC28 | Naučené zvuky | WAV: 3× pípnutí 2 kHz (tón), 2 psí klipy ESC-50 (embedding), pozadí 6 s z mikrofonu, test nahrávek, událost s profilem | tón 2003 Hz; cizí WAV, špatný název a jiný typ odmítnuty; pozadí pod prahem a prah pod shodou vzorků; pípnutí a jiný pes nad prahem, 900 Hz a déšť/tleskání/ohňostroj pod ním; událost s profilem uložena, neplatný profil odmítnut; po testu nic nezůstane |
| TC29 | Volby MQTT | testovací mosquitto (`--mqtt-test host:dir`: 18883, TLS 8883, ws 1884, wss 8884, vlastní CA) | neplatné hodnoty 400; ws připojí; TLS s veřejnou CA, cizí CA a jménem ≠ IP odmítnuto s popisem; vlastní CA bez kontroly jména a wss připojí; client ID, keepalive 20 s, základní topic, stav po 10 s; discovery vypnuto odebere entity, `homeassistant/status online` je pošle znovu; původní nastavení obnoveno |
| TC31 | Akustický tlak v dB(A) | `/api/sound/status` spl, `POST /api/sound/config` (`spl_reference`, `spl_offset`, `spl_reset`) | úrovně LAeq 20–110 dB(A), LAmax ≥ LAeq; referenční údaj mimo rozsah 400; po kalibraci ukazuje LAS referenci (±1,5 dB) a posun se posunul o rozdíl; přímý posun platí bez restartu; reset vrátí odhad 135 dB; původní kalibrace obnovena |
| TC30 | Rozhraní integrace Home Assistant | mDNS dotaz `_p4gw._tcp`, WebSocket `/api/ws`, dočasná událost na třídu Silence, heslo | mDNS odpoví s MAC brány; stav všech povolených událostí dorazí v jedné dávce celý a detekce ihned; bez hesla handshake 401, s heslem 101; události i heslo obnoveny |
| TC22 | Zigbee síť přes Zigbee2MQTT | `GET /api/zigbee/net`, zavření připojování | instance nalezena na brokeru, seznam zařízení s koordinátorem, Zigbee2MQTT odpoví na operaci, neznámá operace 400 |
| TC21 | Mapa Thread sítě | `GET /api/thread/topology` v roli Thread BR | routery v mapě, tato brána jako border router se svými linkami, opakovaný dotaz funguje |
| TC19 | Telemetrie C6 ve všech rolích brány | Thread BR, Zigbee koordinátor s TCP klientem (požadavek GET_MODULE_VERSION), Zigbee router | v každé roli `c6.telemetry` s teplotou a RAM; žádný výpadek RCP; k hostiteli Zigbee nedorazí rámec telemetrie (typ 0x7e), všechny rámce mají platné CRC a NCP odpoví na požadavek |

Jednotkový test váhového filtru A (bez desky, v CI): `tests/unit/aweight_test.c`, příkaz v hlavičce souboru.

Jednotkový test filtru rámců (bez desky): `cc -std=c11 -Wall -Wextra -Werror -o /tmp/zb_filter_test tests/unit/zb_filter_test.c && /tmp/zb_filter_test`
| TC16 | Oddělené Thread sítě rolí | router se připojí do testovací sítě, pak zpět na BR | border router zůstane ve své síti |

## Integrace Home Assistant (bez desky)

[ha_integration/test_p4gw.py](ha_integration/test_p4gw.py): pytest-homeassistant-custom-component proti
falešné bráně (REST a WebSocket na 127.0.0.1), běží i v CI.
`pip install -r tests/ha_integration/requirements.txt && python -m pytest tests/ha_integration`

Ověřuje: config flow ručně, s heslem (špatné heslo), nedostupná brána, zeroconf (potvrzení, změna
adresy u známé brány, záznam bez MAC odmítnut); entity a jejich stavy, entity jen pro povolené
události; push detekce a stavu události, push průběhu aktualizace; nová událost přidá entity bez
reloadu, vypnutá je nedostupná; příkazy (přepínač zvuku, role, restart, instalace); obnovení push
spojení; heslo nastavené později spustí reauth.

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
| UI06 | jazyk CZ, všechny záložky (i všechny položky menu přeložené), pak EN | záložky a texty česky (`Přehled`), jména zvuků zůstanou anglicky, události mají český název (i v „Právě slyším“), volba přežije reload, EN vrátí původní texty |
| UI12 | Zigbee › Síť a zařízení (je-li Zigbee2MQTT) | instance, všechna zařízení v tabulce, detail po najetí, mapa s uzly, je-li uložená |
| UI13 | Zigbee router: vysílací výkon a Identify | pole ukazuje výkon routeru a při psaní ho obnova stránky nepřepíše, nastavená hodnota se vrátí ve stavu routeru, Identify rozbliká ikonu v hlavičce a po 10 s zmizí, řádek brány má tlačítka výkonu a Identify |
| UI14 | Thread › Topologie (je-li role Thread) | najetí na router v mapě ukáže detail v levém horním rohu mapy a zvýrazní jeho linky, po odjetí zvýraznění zmizí; legenda nezakrývá uzly ani detail; seznam zařízení bublinu nezobrazuje (UI12 totéž pro Zigbee) |
| UI16 | menu podle role | stránky neaktivní role jsou v menu zavřené, odkaz na ně otevře stránku, která funguje; sekce je zavřená jen když nefunguje žádná její stránka |
| UI17 | mapa sítě: zoom a rozložení | kolečko a +/− přibližují, − nejdál na celou mapu, domeček vrátí výchozí pohled, L rozmístí uzly podle signálu (pamatuje si to) a zpět na kruh, legenda nezakrývá uzly |
| UI18 | teplota C6 | na Přehledu má teplota C6 stejně jako P4 rozsah (min, max), pokud ji firmware C6 hlásí |
| UI19 | log před restartem | Systém › Diagnostika: tlačítko stáhne log předchozího běhu (`*-previous.log`) |
| UI20 | záloha nastavení | export stáhne JSON bez hesel, cizí soubor skončí chybou, potvrzení importu uvádí původ a zrušení nic nemění |
| UI21 | zálohy Zigbee | mimo roli Zigbee je stránka zavřená; v roli Zigbee se výběr automatické zálohy uloží a funguje Zálohovat teď |
| UI22 | online aktualizace | uložení URL, zobrazení chyby kontroly, Instalovat zakázáno bez nabídky, vymazání URL |
| UI23 | naučené zvuky | stránka se načte, cizí soubor skončí chybou, test nahrávky ukáže skóre |
| UI24 | MQTT | volby TLS jen pro mqtts:// a wss://, pole certifikátu jen pro vlastní CA, pokročilé volby |
| UI25 | log | každý řádek začíná místním časem prohlížeče; úroveň, zdroj, tag (i kliknutím) a hledání (text se zvýrazněním, /regex/) filtrují |
| UI15 | nápovědy (i) | žádný klíč nápovědy není v TIPS dvakrát; tabulky zařízení Thread a Zigbee a údaje instance Zigbee2MQTT mají vlastní nápovědy, ne texty jiných tabulek se stejným názvem sloupce |
| UI11 | průběh flashování C6 (podvržený stav) | štítek „Flashování C6 · 46 %“ v hlavičce i mimo Přehled, fáze v bublině, pruh pod hlavičkou, žádný banner; modální okno zamkne stránku, ukáže fázi, procenta a odhad zbývajícího času, Escape ho nezavře; po skončení vše zmizí |
| UI10 | štítek v hlavičce a karta Home Assistant a Matter | v roli Thread BR / Zigbee koordinátor je štítek uživatele brány, jinak skrytý; Nastavení najde HA přes mDNS, „Použít“ vyplní ws://IP:5580/ws, test Matter Serveru odpoví |
| UI09 | Systém › Síťové okolí (je-li uložen sken) | 16 hodnocených kanálů, spektrum, doporučení, Wi-Fi tabulka po jednom řádku na BSSID, rychlý sken jen v roli Thread |
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
| M09 | Online aktualizace: instalace | manifest a p4gw.bin na HTTP serveru v LAN, Instalovat 4× za sebou | stáhne se do druhého gateway slotu, restart `software`, slot potvrzen, ESPHome slot nedotčen; chybějící obraz, c6fw.bin a cizí aplikace odmítnuty bez zápisu |
| M10 | Naučené zvuky z mikrofonu | reproduktor u desky: nahrát 5 vzorků zvuku a pozadí, vytvořit událost, přehrát zvuk | událost v logu a v HA; jiný zvuk ji nespustí |
