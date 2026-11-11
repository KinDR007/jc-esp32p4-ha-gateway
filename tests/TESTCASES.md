# Hardwarové testy P4 gateway

Automatizace: [hw_test.py](hw_test.py). Testy běží proti skutečné desce v LAN a jsou destruktivní:
přepínají role, flashují C6 a zapisují slot gateway.

```sh
# výpis testů
python3 tests/hw_test.py --list
# celý běh
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
| TC09 | ESPHome na rádiu | role `esphome` na rádiu B (bez něj na A) přes `/api/role` | rádio dostane bundle `esphome`, odpoví přes Improv (zařízení); s Wi-Fi ze stránky ESPHome se připojí a hlásí URL, bez ní „not provisioned“; role rádia zpět |
| TC10 | OTA gateway | upload `dist/p4gw.bin` | zápis do neaktivního slotu, start v `pending_verify`, po 60 s `valid`; gateway image do slotu ESPHome odmítnut |
| TC11 | Přihlášení účtem | staré heslo přes `/api/settings`, účet správce, přístup bez/se/se špatným heslem (Basic), smazání | staré heslo 400; 401 / 200 / 401; po smazání účtu otevřená brána |
| TC12 | Neplatné uploady | nesmysl do c6fw a gateway, pak platný c6fw | 400, uložené image nepoškozené, 4 balíky zpět |
| TC13 | Vynucený reflash C6 | `POST /api/c6/reflash` | po restartu běží stejná role, v logu `C6 now runs`; se sloty se znovu zapíše celé rozložení |
| TC14 | Zvuková služba | zapnout mikrofon a rozpoznávání | zachytávání bez chyb, model načten, inference pod 150 ms, CPU jádra 1 pod 40 %, klip WAV 2 s |
| TC15 | Rozpoznávání zvuků (`--play`) | `say` a ESC-50 štěkot z reproduktoru počítače u desky | Speech ≥ 0,6, Dog nebo Bark ≥ 0,5 |
| TC17 | Konfigurace událostí | neplatné ID / třída / duplicita, nová událost a výjimka, restart, předvolba | 400 pro chyby, vše přežije restart, předvolba použita, původní konfigurace obnovena |
| TC18 | Událost z přehraného zvuku (`--play`) | kočka z ESC-50, pak s ignorovanými třídami kočky | událost `cat` vznikne, s ignorovanými třídami ne |
| TC20 | Síťové okolí | rychlý sken v roli Thread BR, pak úplný sken (restart, skenovací firmware C6) | rychlý: 16 kanálů, vlastní Thread síť nalezena, bez výpadku RCP; úplný: skener odpoví, 16 kanálů, Wi-Fi bez duplicit, u každého AP šířka a známé zabezpečení (WPA3 ne jako otevřená), vlastní PAN v provozu; role a Thread síť po skenu zpět; s rádiem B (C5) i úplný sken na B: sítě 5 GHz, role B zpět |
| TC23 | Sloty C6 | přepnutí thread_br → zigbee_router → thread_br | c6fw obsahuje balíček `slots`, rozložení se znovu nezapisuje, vybere se jen slot (otadata), router se vrátí do své sítě, Thread se připojí, každé přepnutí do minuty |
| TC24 | Log před restartem | restart přes API | `/api/logs/previous` vrátí log předchozího běhu končící požadavkem na restart, aktuální log ho neobsahuje |
| TC25 | Záloha nastavení | export, chybné importy, import se změnou a obnova | export bez stavu desky a hesla jen se `secrets=1`; neplatný base64, neznámá role a cizí formát odmítnuty bez změny; import platí po restartu, klíč stavu desky v souboru ignorován (C6 se nepřeflashuje); původní nastavení obnoveno |
| TC26 | Záloha Zigbee | thread_br → zigbee_router, záloha na požádání, zpět | v roli Thread odmítnuto (409), interval jen 0/1/7; záloha zvýší generaci a uloží čas, nic se neflashuje, router se vrátí do sítě; odchod z role dál ukládá snapshot |
| TC27 | Online aktualizace: kontroly | neplatná URL, kontrola a instalace bez nabídky, chybějící manifest, ne-manifest, HTTPS | odmítnutí 400/409, čitelné chyby, HTTPS projde TLS (404 ze serveru), instalace bez nabídky odmítnuta |
| TC28 | Naučené zvuky | WAV: 3× pípnutí 2 kHz (tón), 2 psí klipy ESC-50 (embedding), pozadí 6 s z mikrofonu, test nahrávek, událost s profilem | tón 2003 Hz; cizí WAV, špatný název a jiný typ odmítnuty; pozadí pod prahem a prah pod shodou vzorků; pípnutí a jiný pes nad prahem, 900 Hz a déšť/tleskání/ohňostroj pod ním; událost s profilem uložena, neplatný profil odmítnut; po testu nic nezůstane |
| TC29 | Volby MQTT | testovací mosquitto (`--mqtt-test host:dir`: 18883, TLS 8883, ws 1884, wss 8884, vlastní CA) | neplatné hodnoty 400; ws připojí; TLS s veřejnou CA, cizí CA a jménem ≠ IP odmítnuto s popisem; vlastní CA bez kontroly jména a wss připojí; client ID, keepalive 20 s, základní topic, stav po 10 s; discovery vypnuto odebere entity, `homeassistant/status online` je pošle znovu; původní nastavení obnoveno |
| TC31 | Akustický tlak v dB(A) | `/api/sound/status` spl, `POST /api/sound/config` (`spl_reference`, `spl_offset`, `spl_reset`) | úrovně LAeq 20–110 dB(A), LAmax ≥ LAeq; referenční údaj mimo rozsah 400; po kalibraci ukazuje LAS referenci (±1,5 dB) a posun se posunul o rozdíl; přímý posun platí bez restartu; reset vrátí odhad 135 dB; původní kalibrace obnovena |
| TC32 | LAN a provoz | `/api/status` links a eth, `/api/net/traffic`, `/api/net/lan`, `/api/net/lan/confirm` | provoz webu se započítá a součty sedí; chybná maska, brána mimo podsíť, chybějící adresa a neznámý režim linky 400; potvrzení bez změny 409; statická adresa bez potvrzení se do 140 s vrátí; po potvrzení uložena a internet dostupný; původní nastavení obnoveno |
| TC33 | Plný restart | `POST /api/reboot {"full": true}` | role naběhne znovu; doba běhu C6 odpovídá P4 (±30 s), C6 se tedy restartoval s P4 |
| TC34 | Uživatelé a oprávnění | `/api/users`, `/api/auth/*`, `/api/tokens` | první účet jen správce a bez vynucené změny hesla; poté 401 bez přihlášení; chybné heslo 401; relace; krátké heslo 400; nový uživatel musí změnit heslo (do té doby 403); divák nevidí klíč Thread, nemění přehled, nemá správu (403); vlastní úroveň uživatele přebije roli; Basic jen bez 2FA; token s rolí (divák nerestartuje), neznámý token 401; TOTP: QR, chybný kód 400, zapnutí s 10 záložními kódy, heslo samo vyžádá kód, přihlášení kódem a záložním kódem (jen jednou); zámek po 5 pokusech; žádost o reset, správce nastaví dočasné heslo, vypne 2FA, odemkne, staré relace končí; posledního správce nejde odebrat; úklid do otevřeného režimu |
| TC35 | Telegram bot | `/api/telegram`, `/api/auth/telegram`, `/pair` | chybný token a prázdné pásmo 400; neplatný token dojde přes TLS k Telegramu a vrátí 401; bot zase vypnutý; vlastní nastavení a párování bez uživatele/běžícího bota 409 |
| TC36 | Online aktualizace C6 | `/api/update/check`, `/api/update/install {"target":"c6"}` s vlastním HTTP serverem testu | balík s jiným popisem nabídnut; chybný sha256 odmítnut bez restartu; nový balík po restartu uložen, role zpět, C6 nepřepsán (stejné firmwary); původní balík vrácen |
| TC37 | Seznam vydání (rollback) | `/api/update/check` s `releases.json` vlastního HTTP serveru testu, `/api/update/install {"version"}` | nabídnuta jen vydání s layoutem desky; vydání jiného layoutu i neuvedené odmítnuto (409); manifest bez layoutu odmítnut bez restartu, nabídka obnovena; poslední vydání jiného layoutu nenabídnuto |
| TC39 | Dvě rádia | rádio A `thread_br`, rádio B (XIAO ESP32-C5 na JP1) `zigbee_coordinator` přes `/api/role {"radio":"b"}` | obě role běží; Thread síť na A; telemetrie NCP z B, čistá linka; TCP klient na 6638; druhá síť Thread i Zigbee odmítnuta (409); rádio B se nerestartuje. Bez rádia B přeskočeno |
| TC41 | Wi-Fi uplink na rádiu B | rádio B `wifi_uplink` vedle Thread BR na A, LAN zapojená | Wi-Fi připojená s vlastní adresou; výchozí trasa i páteř Thread na Ethernetu; web přes adresu Wi-Fi odpovídá, propustnost > 30 kB/s (zaznamenaná), stránka započítaná v provozu Wi-Fi; heslo v API není; sken vrátí sítě; druhý uplink na A odmítnut (409). Bez rádia B SKIP |
| TC42 | Výpadek LAN za běhu | `POST /api/net/lan/outage {"seconds":150}` | brána do 30 s dostupná přes Wi-Fi s výchozí trasou; internet přes Wi-Fi; Thread BR se restartem přesune na páteř Wi-Fi a síť běží; po návratu LAN výchozí trasa hned na Ethernetu, páteř zpět na Ethernet po hysterezi 5 min |
| TC43 | Chybná síť Wi-Fi | `POST /api/net/wifi` s neexistující SSID, pak `{"ssid":""}` | stav `disconnected` s „network not found“, LAN beze změny; po zrušení vlastního nastavení zpět na síti ESPHome; heslo < 8 znaků a statika bez masky 400 |
| TC44 | Nastavovací přístupový bod | neexistující síť + výpadek LAN 300 s, Mac (`--wifi-iface en0`) se připojí k AP pojmenovanému jako brána | AP otevřený, adresa 192.168.4.x, DNS i brána 192.168.4.1; každé jméno se přeloží na bránu; `/generate_204` 302 na web; API přes AP s počtem klientů; síť nastavená přes AP se připojí, AP se po připojení zavře. Bez Wi-Fi rozhraní testu SKIP |
| TC46 | Skupiny MQTT discovery | anonymní mosquitto (`--mqtt-broker host:port`) | neplatné `discovery_groups` 400; všechny skupiny: entity role, uptime, firmware a I2C senzoru (vlastní seznam availability) publikované; jen `sound_triggers`: tyto entity i binární senzor události odebrány (prázdný retained config), spouštěč zvukové události zůstává; zpět všechny skupiny; původní nastavení obnoveno importem |
| TC47 | I2C senzory | `POST /api/sensors/scan`, `PUT /api/sensors`, nepřipojený BME280 na 0x76/0x77 | sběrnice na GPIO7/8; sken najde kodek ES8311 na 0x18 jako součást desky a nepovažuje ho za senzor; adresa kodeku, adresa mimo typ, neznámý typ, interval 1 s, dvojí adresa a jméno nad 32 znaků 400; jméno s diakritikou uloženo; nepřipojený senzor do 15 s ve stavu chyba bez hodnot; skutečně připojené senzory dají hodnoty ve věrohodném rozsahu; mikrofon běží jako předtím bez chyb čtení; původní konfigurace obnovena |
| TC48 | Modbus (TCP simulátor) | `tools/modbus/sim_tcp.py` spuštěný testem na tomto počítači, 4 šablony + nedostupné zařízení; s `--mqtt-broker` i MQTT | UART3 na GPIO26/27; vestavěné šablony; neplatná konfigurace 400; všechny hodnoty SDM630, PZEM-016, DTSU666, PZEM-017 sedí s nezávislým kódováním simulátoru; zápis number, select, button s funkcí 0x42 přečten zpět; nezapisovatelné, mimo rozsah, neznámá volba a neznámé zařízení 400; ruční čtení a výjimka 2; hledání najde jednotky 1-4; nedostupné zařízení offline bez hodnot; vlastní šablona uložena a čte, id vestavěné odmítnuto, používaná a vestavěná nesmazána; MQTT: zařízení samostatné s via_device, number s limity, tlačítko, zápis přes MQTT přečten zpět; konfigurace obnovena |
| TC49 | Hledání Modbus TCP v síti | `tools/modbus/sim_tcp.py` (Victron GX jednotka 100, VE.Bus 228, SDM630 jednotka 1) spuštěný testem | výchozí síť brány; neplatná síť, prefix pod 22, 5 portů, jednotka 300 → 400; simulátor nalezen (/32): jednotka 1 výjimka 2, poznán Victron GX podle sériového čísla, šablona, jednotky 100 a 228; web brány (port 80) otevřený, ne Modbus, nevypsán; druhé hledání při běžícím 409, zastavení; šablony victron_gx_system a victron_vebus čtou všechny hodnoty podle kódování simulátoru; konfigurace obnovena |
| TC50 | Modbus přes UDP (moduly Wi-Fi GoodWe) | `tools/modbus/sim_tcp.py --udp --split` (goodwe_xs jednotka 127, porty 8899 a 48899) spuštěný testem | hledání s UDP najde modul s jednotkou 127 a odpovědí na 48899; ruční dotaz `udp` přečte sériové číslo; neexistující jednotka bez odpovědi (-1); `udp` bez host a neznámý transport → 400; zařízení na `udp` uloženo s výchozím portem 8899 a šablona goodwe_xs čte všechny hodnoty (odpovědi rozdělené do dvou datagramů); konfigurace obnovena |
| TC51 | Čas brány (Systém › LAN › Čas) | `GET`/`POST /api/time`, restart | SNTP synchronizováno; 4 servery, mezera ve jménu, servers ne seznam, tz_name bez tz, prázdné tz a tz 64 znaků → 400; nové servery (tik.cesnet.cz, 192.0.2.1) a UTC: znovu synchronizováno bez restartu (do 5 s od počítače); pásmo v `/api/status` a nastavení Telegramu; staré pole `tz` Telegramu nastaví pásmo bez názvu; prázdný seznam = bez synchronizace; server a pásmo vydrží restart; nastavení obnoveno |
| TC52 | GPIO na JP1 bez propojky | `GET`/`PUT /api/gpio`, `POST /api/gpio/set`, restart | 26 pinů, piny rádia A (a rádia B s rolí) zamčené, GPIO1/2/3/45 volné; odmítnuto: pin rádia A, rádia B, GPIO mimo JP1, dvakrát, neznámý režim, PWM 2 MHz, debounce 2 s, jméno 33 znaků, neznámé expose, filtr 20 µs; vstupy čtou podle pull-up/pull-down; výstup čten zpět na pinu, inverze, pulz 0,8 s; open drain s pull-down pin pustí (0), push-pull ho drží (1); PWM 1 kHz 33 % 16 bitů; chyby set 400/404; čítač 0 a reset; po restartu výstup poslední 1, PWM posledních 33 %, invertovaný výstup zapnutý; konfigurace obnovena |
| TC53 | GPIO přes MQTT | `--mqtt-broker` (anonymní mosquitto), skupina `gpio` | zařízení „<host> GPIO“ přes bránu: switch, binary_sensor, light (jas 0–100 = střída), senzory počet a frekvence; stav retained; příkazy ON, PULSE 1500 (zapne a vypne), střída 30, light OFF → 0; pásek WS2812 jako JSON light (RGB, duha) nastavený zprávou na `light/set`, stav na `gpio/1/light`; změna režimu a uvolnění pinu odebere staré entity a publikuje nové; piny vyměněné při vypnutém MQTT a následný restart: staré entity odebrány po připojení; skupina gpio vypnutá entity odebere; piny se nastavují až na testovacím brokeru; nastavení obnoveno importem |
| TC54 | Funkce GPIO přes periferie bez propojky | `PUT /api/gpio`, TCP 6639, restart | odmítnuto: UART bez rx, rx = tx, rx na rádiu A, port 6638, 100 Bd, dva UARTy, rx použitý znovu, 1000 LED, 1-Wire každou 1 s, I2C 250 kHz; most UART: TCP klient pošle 18 B do UARTu, druhý klient prvního nahradí (zavřen), odpojení; pásek: neplatná barva, efekt a jas odmítnuty, barva a jas ho zapnou, duha; 1-Wire bez zařízení (present false), hledání; po restartu pásek zapnutý se svou barvou a duhou, most znovu přijímá; druhá sběrnice I2C na GPIO2/3 400 kHz, senzor na sběrnici 2 s id i2c2_76, sběrnice 3 odmítnuta, zrušení pinů sběrnici odebere; konfigurace obnovena |
| TC57 | Emulace (virtuální zařízení) | `tools/modbus/sim_tcp.py` (eastron_sdm630 jednotka 1 na TCP 1510) spuštěný testem | neplatný zdroj (neznámý, chybí, sám sebe) → 400; virtuální abb_b23 ze SDM630: stav ok, 29 z 30 namapováno (power ← total_power, type pevný, firmware bez zdroje), hodnoty jako u zdroje, zápis přes API → 400; přes server na portu 1511 čtení 0x8960 x6, 0x8900 x2, 0x5B00 x62, 0x5000 x24, 0x5460 x24, 0x8908 x8 registr po registru jako kódování šablony ABB nezávisle v testu (mezera = 0xFFFF, mimo rozsah = nan); typ „B23 112-100“; zápis → výjimka 1; žádné čtení neprošlo ke zdroji; zdroj zastaven → výjimka 11; konfigurace obnovena |
| TC56 | Šablony ABB B21, B23/B24 | `tools/modbus/sim_tcp.py --lenient` (abb_b21 jednotka 1, abb_b23 jednotka 2 na TCP 1509) spuštěný testem | všech 14 a 30 hodnot jako zakódované (včetně energií u64); zápis „bez hodnoty“ do simulátoru (u64 samé jedničky na 0x5004, s16 0x7FFF na 0x5B3A, s32 0x7FFFFFFF na 0x5B16) → hodnota zmizí, ostatní zůstanou; konfigurace obnovena |
| TC55 | Server Modbus TCP | `tools/modbus/sim_tcp.py --log-reads` (eastron_sdm630 jednotka 1 na TCP 1504) a `--udp` (goodwe_xs 127) spuštěné testem | neplatná nastavení → 400 (neznámé zařízení, unit 248, port 80, vlastní port = sdílený, ani unit ani port, rate 0, allow ne adresa, write 1, unit dvakrát, sdílený port 8081); porty 502, 1505, 1506 otevřené; 502 s unit 10 vrátí napětí jako zařízení, vlastní port s unit 255 také, a to z cache (dotaz na zařízení nedojde); unit 11 → výjimka 10; neznámý registr projde na zařízení a vrátí jeho výjimku 2; rate 3/s → výjimka 6; zápis vypnutý → 1, adresa mimo allow → 1, povolená adresa → zápis dojde do simulátoru; třetí klient na portu odmítnut, stav ukáže 2 klienty; počítadla; GoodWe přes UDP odpoví na portu 1506 jako Modbus TCP (sériové číslo); stejné neplatné čtení podruhé z cache (výjimka 2); pomalé zařízení (`--delay 300 --lenient`, port 1507): druhé čtení z cache, po 2,2 s znovu na zařízení, zápis zahodí blok a další čtení vrátí novou hodnotu, dva klienti se stejným čtením = jedno čtení zařízení, během pollu klient čeká < 1 s (jedna transakce), `cache_ms` 1000/6000 → 400, `cache_ms` 0 pošle každé čtení na zařízení; konfigurace obnovena |
| TC45 | Prohození Wi-Fi mezi rádii | `POST /api/role {"swap":true}` dvakrát | Wi-Fi uplink na rádiu A (C6) s adresou a webem; rádio B běží původní roli A (Thread BR); po prohození zpět je rádio A ve své původní Thread síti |
| TC40 | Thread síť per rádio | rádio A `zigbee_router`, rádio B `thread_br`, pak rádio B vypnuto a A zpět `thread_br` | rádio B (RCP na C5) má vlastní síť (jiné ext PAN ID), rádio A se vrátí do své původní sítě |
| TC30 | Rozhraní integrace Home Assistant | mDNS dotaz `_p4gw._tcp`, WebSocket `/api/ws`, dočasná událost na třídu Silence, účet a token | mDNS odpoví s MAC brány; stav všech povolených událostí dorazí v jedné dávce celý a detekce ihned; bez hesla handshake 401, s heslem 101; události i účet a token obnoveny |
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
spojení; heslo nastavené později spustí reauth. GPIO: zařízení „<host> GPIO“ přes bránu se vstupem,
výstupem, PWM jako světlem (jas = střída, zapnutí bez jasu vrátí poslední střídu) a dvěma senzory čítače;
příkazy na `/api/gpio/set`, push tématu `gpio`; pin jen pro MQTT bez entity; vypnutý pin a změna režimu;
firmware bez `/api/gpio`; pásek WS2812 jako RGB světlo s efekty, teploměry 1-Wire (čidlo nalezené později přibude, zmizelé je nedostupné).

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
| UI16 | menu podle role | stránky sítě, kterou neprovozuje žádné rádio (Thread, Zigbee, ESPHome), jsou v menu zavřené nebo sekce zešedne, odkaz na ně otevře stránku, která funguje; sekce je zavřená jen když nefunguje žádná její stránka |
| UI17 | mapa sítě: zoom a rozložení | kolečko a +/− přibližují, − nejdál na celou mapu, domeček vrátí výchozí pohled, L rozmístí uzly podle signálu (pamatuje si to) a zpět na kruh, legenda nezakrývá uzly |
| UI18 | teplota C6 | na Přehledu má teplota C6 stejně jako P4 rozsah (min, max), pokud ji firmware C6 hlásí |
| UI19 | log před restartem | Systém › Diagnostika: tlačítko stáhne log předchozího běhu (`*-previous.log`) |
| UI20 | záloha nastavení (Systém › Údržba a záloha) | export stáhne JSON bez hesel, cizí soubor skončí chybou, potvrzení importu uvádí původ a zrušení nic nemění |
| UI21 | zálohy Zigbee | stránka je otevřená v každé roli (tabulka uvádí rádio zálohy); výběr automatické zálohy se uloží; Zálohovat teď jen když některé rádio běží jako Zigbee |
| UI22 | online aktualizace | uložení URL, zobrazení chyby kontroly, nabídka staré URL zmizí (Instalovat chybí nebo je zakázané), vymazání URL |
| UI23 | naučené zvuky | stránka se načte, cizí soubor skončí chybou, test nahrávky ukáže skóre |
| UI24 | MQTT | volby TLS jen pro mqtts:// a wss://, pole certifikátu jen pro vlastní CA, pokročilé volby; skupiny discovery včetně `sensors`, předvolby |
| UI25 | log | každý řádek začíná místním časem prohlížeče; úroveň, zdroj, tag (i kliknutím) a hledání (text se zvýrazněním, /regex/) filtrují |
| UI26 | system/wifi, radios/roles, overview | pole hesla prázdné; karta stavu uplinku (nebo „žádné rádio“); statika odemkne pole adres, DHCP je zamkne; provoz přes Wi-Fi; sken naplní SSID; skupina rolí Wi-Fi a tlačítko prohození jen s uplinkem; schéma přehledu ukazuje Wi-Fi |
| UI27 | Rádia › Role | přepínač zobrazení tabulka / seznam / dlaždice, volba zůstane po obnovení |
| UI28 | Toky dat | zdroje, služby a odběratelé s čarami, tabulka toků s odkazy |
| UI29 | průvodce nastavením | každý krok otevře svou stránku, přežije obnovení, dokončení zavře |
| UI30 | Systém › API | všechny endpointy z /api/help, filtr, příklady s adresou a tokenem, Vyzkoušet |
| UI33 | Systém › Hledání Modbus TCP | položka menu, předvyplněná síť brány, neplatná síť odmítnuta, hledání samotné brány (port 80 otevřený, ne Modbus) doběhne a nic nevypíše |
| UI34 | Hledání Modbus TCP: modul GoodWe na UDP | simulátor na UDP 8899 vypsán s jednotkou 127, Přidat ho založí na připojení `udp` se šablonou goodwe_xs, RS485 ho ukáže jako `UDP host:8899 #127`; konfigurace obnovena |
| UI35 | Systém › LAN › Čas | servery se stavem, Jiné ukáže pole POSIX, zóna ze seznamu uložena s pravidlem POSIX (America/New_York), hodiny v záhlaví i log v pásmu brány, Telegram bez vlastního pásma odkazuje na LAN; nastavení obnoveno |
| UI36 | Systém › GPIO (JP1) | konektor 26 pinů, GPIO4 zamčený s důvodem, GPIO2 nastavený jako výstup se zapne (pin čte 1) a pulzuje 1 s, GPIO1 PWM 2 kHz sleduje posuvník (25 %), uvolnění pinu; konfigurace obnovena |
| UI37 | Systém › Server Modbus TCP | Přidat zařízení s unit id 77, vlastním portem 1520, limitem 4/s, cache 3000 ms, zápisem a povolenou adresou; Uložit to uloží, stav ukáže otevřené porty 502 a 1520; odebrání seznam vyprázdní; konfigurace obnovena |
| UI38 | Systém › RS485: emulace | připojení Emulace skryje adresu a host, nabídne zdroj; poznámka o dvojích hodnotách v HA; přidání uloží transport virtual, source a šablonu; řádek „emuluje <zdroj>“, „29 z 30 hodnot ze zdroje“; rozbalení ukáže původ hodnot (total_power, import_energy_l1, pevná, bez hodnoty); konfigurace obnovena |
| UI32 | Systém › RS485 | počet šablon, rychlost sběrnice, poznámka šablony s výchozími parametry; přidání zařízení ze šablony, přejmenování s diakritikou a MQTT uloženy, detail všech entit, odebrání; kopie šablony v editoru, neplatná odmítnuta s chybou, platná uložena a smazána; ruční dotaz bez zařízení „no answer“; konfigurace obnovena |
| UI31 | Systém › Hardware | zapojení CN3, sken ukáže kodek 0x18, seznam podporovaných typů; ruční přidání nabídne jen typy pro adresu; jméno s diakritikou a Exponovat uloženy; stav chybějícího senzoru; odebrání po potvrzení; původní konfigurace obnovena |
| UI15 | nápovědy (i) | žádný klíč nápovědy není v TIPS dvakrát; tabulky zařízení Thread a Zigbee a údaje instance Zigbee2MQTT mají vlastní nápovědy, ne texty jiných tabulek se stejným názvem sloupce |
| UI11 | průběh flashování C6 (podvržený stav) | štítek „Flashování C6 · 46 %“ v hlavičce i mimo Přehled, fáze v bublině, pruh pod hlavičkou, žádný banner; modální okno zamkne stránku, ukáže fázi, procenta a odhad zbývajícího času, Escape ho nezavře; po skončení vše zmizí |
| UI10 | štítek v hlavičce a karta Home Assistant a Matter | v roli Thread BR / Zigbee koordinátor je štítek uživatele brány, jinak skrytý; Home Assistant › Matter najde HA přes mDNS, „Použít“ vyplní ws://IP:5580/ws, test Matter Serveru odpoví |
| UI09 | Rádia › Rádiové okolí (je-li uložen sken) | 16 hodnocených kanálů v zónách pod Wi-Fi 1/6/11 a v mezerách, spektrum, doporučení, Wi-Fi tabulka po jednom řádku na BSSID s AP seskupenými podle BSSID, spektrum 5 GHz jen se sítěmi 5 GHz, rychlý sken jen v roli Thread |
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
