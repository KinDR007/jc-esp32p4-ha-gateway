# Zadání: Thread Border Router na Guition JC-ESP32P4-M3-DEV

## Cíl

Síťový (LAN) Thread Border Router pro Home Assistant postavený na desce Guition
JC-ESP32P4-M3-DEV:
- ESP32-P4 je host (OpenThread border router, backbone přes Ethernet).
- Integrované ESP32-C6 slouží jako 802.15.4 RCP.

Router má podporovat Matter-over-Thread a integrace HA `OpenThread Border Router` a `Thread`.

## Závěr analýzy

Řešení je proveditelné, ale jen mimo ESPHome. ESPHome komponenta `openthread` umí pouze Thread
zařízení, border router neumí ([esphome.io/components/openthread](https://esphome.io/components/openthread/)).
Firmware proto bude čisté ESP-IDF nad
[esp-thread-br](https://github.com/espressif/esp-thread-br).

Kombinace „P4 + Ethernet jako host, C6 jako RCP“ je od esp-thread-br v1.2 oficiálně podporovaná.
Dokumentace ji popisuje na ESP32-P4-Function-EV-Board
([README_esp32p4](https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md)).
Guition deska má stejný Ethernet PHY (IP101) i stejný koprocesor (C6). Liší se jen zapojení
mezi P4 a C6.

**Jedna deska zastane jen jednu roli.** Na C6 nelze současně provozovat BLE scan (BLE proxy)
a Thread router, matice koexistence tuto kombinaci nepodporuje
([ESP-IDF coexist, ESP32-C6](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html)).
BLE proxy proto zůstane na jiné desce. Pro P4 je hotová konfigurace v projektu `esphome/ble_proxy`.

Varianty, které jsem zamítl:
- **Spinel přes esp-hosted (SDIO):** esp-hosted tento transport pro RCP zatím nepodporuje
  ([esp-hosted-mcu examples/openthread](https://github.com/espressif/esp-hosted-mcu/tree/main/examples/openthread/border_router)).
- **C6 jako USB RCP pro HA add-on:** žádný z USB-C portů nevede na C6 a výsledek by navíc nebyl LAN router.

## Hardware

Deska: JC-ESP32P4-M3-DEV, modul JC-ESP32P4-M3 (ESP32-P4NRW32, 16 MB flash, 32 MB PSRAM, C6 uvnitř
modulu). Schéma je v [docs/](docs/), zdroj
[makergo1024/JC-ESP32P4-M3-DEV-v3.2](https://github.com/makergo1024/JC-ESP32P4-M3-DEV-v3.2/tree/main/5-Schematic).

### Ethernet (IP101, ověřeno ze schématu a ESPHome konfigurace)

| Signál | P4 GPIO |
|---|---|
| MDC | 31 |
| MDIO | 52 |
| PHY_RSTN | 51 |
| RMII_CLK (50 MHz z PHY, ext. in) | 50 |
| TXD0 / TXD1 / TX_EN | 34 / 35 / 49 |
| RXD0 / RXD1 / CRS_DV | 29 / 30 / 28 |
| PHY adresa | 1 |

### Propojení P4 ↔ C6

Uvnitř modulu jsou P4 a C6 propojené přes SDIO (P4 GPIO14–19) a reset (C6 CHIP_PU = P4 GPIO54).
Volné UART linky pro Spinel mezi nimi nejsou. Header JP1 ale vyvádí C6 UART0, BOOT i reset
(list `2_EXPAND_IO&BAT`):

| JP1 pin | Signál | JP1 pin | Signál |
|---|---|---|---|
| 13 | P4 GPIO4 | 20 | C6_U0RXD |
| 15 | P4 GPIO5 | 22 | C6_U0TXD |
| 17 | P4 GPIO20 | 24 | C6_IO9 (BOOT, pull-up 5k1) |
| | | 26 | C6_CHIP_PU (= P4 GPIO54) |

Popisky sítí ve schématu leží nad vodičem, ke kterému patří. Pozor: pin 18 je VCC3V3, pin 16 GND.

**Navržené zapojení (3 propojky na JP1, bez pájení):**
- P4 GPIO4 (UART TX) → C6_U0RXD (JP1 13 → 20)
- P4 GPIO5 (UART RX) ← C6_U0TXD (JP1 15 → 22)
- P4 GPIO20 → C6_IO9 (JP1 17 → 24), aby P4 uměl C6 uvést do bootloaderu a aktualizovat RCP

C6 EN ovládá P4 přes GPIO54 uvnitř modulu, propojku nepotřebuje. Výběr GPIO4/5 odpovídá
výchozímu nastavení esp-thread-br pro P4. GPIO20 není strapping pin.

## Postup

### 1. Příprava a záloha
1. Nainstalovat ESP-IDF ve verzi doporučené aktuálním releasem esp-thread-br (v1.3 = IDF v5.5.2,
   [releases](https://github.com/espressif/esp-thread-br/releases)). Verzi zapsat do projektu.
2. Přes USB-UART adaptér na JP1 (C6_U0TXD/RXD, IO9 na GND, puls CHIP_PU) zálohovat celou flash C6
   příkazem `esptool.py --chip esp32c6 read_flash 0 ALL c6_backup.bin`. Bez této zálohy se nelze vrátit
   k továrnímu esp-hosted firmwaru. Po dobu zálohy držet P4 v resetu nebo v bootloaderu, aby C6 neresetoval.
3. Zálohovat i flash P4.

### 2. Firmware RCP (C6)
1. Sestavit `esp-idf/examples/openthread/ot_rcp` pro `esp32c6`: UART0, výchozí piny C6, 460800 Bd
   (podle konfigurace esp-thread-br).
2. Poprvé nahrát přes JP1 a USB-UART adaptér. Další aktualizace půjdou přes RCP update z P4 (krok 3).

### 3. Firmware hosta (P4)
1. Vyjít z `esp-thread-br/examples/basic_thread_border_router` (target `esp32p4`).
2. Nastavit:
   - Ethernet backbone: IP101, piny podle tabulky výše, `phy_addr` 1, RMII clock ext. in na GPIO50;
   - Wi-Fi vypnout;
   - RCP UART: TX GPIO4, RX GPIO5, 460800 Bd;
   - RCP reset GPIO54, RCP boot GPIO20;
   - `CONFIG_OPENTHREAD_BR_AUTO_START`, `CONFIG_OPENTHREAD_BR_START_WEB` (REST API a web GUI na portu 80);
   - RCP auto-update s firmwarem `ot_rcp` přibaleným do hosta.
3. Ověřit revizi čipu P4 v boot logu (`chip revision`). Od ní se odvíjí target a konfigurace IDF.
4. Flashnout přes USB-C (CH340, UART0 P4).

### 4. Integrace do Home Assistant
1. Ověřit IPv6 na LAN a na hostu HA. Podmínky: RA/RIO akceptované, žádný filtr multicastu,
   u VM nebo Dockeru zapnutý forwarding ([HA Thread](https://www.home-assistant.io/integrations/thread/)).
2. Přidat integraci OTBR s URL `http://<IP>:80` bez koncového lomítka
   ([esp-thread-br HA codelab](https://docs.espressif.com/projects/esp-thread-br/en/latest/codelab/home_assistant.html),
   [python-otbr-api#61](https://github.com/home-assistant-libs/python-otbr-api/issues/61)).
3. V integraci Thread nastavit síť jako preferovanou a poslat credentials do telefonu přes HA Companion.
4. Uvést do provozu testovací Matter-over-Thread zařízení.

### 5. Akceptační testy
- [ ] `ot state` je `leader`, na LAN se objevuje RA s prefixem Thread sítě
- [ ] `ping6` z hosta HA na Thread zařízení
- [ ] SRP → mDNS: služby Thread zařízení jsou vidět na LAN (`_matter._tcp`)
- [ ] NAT64: Thread zařízení dosáhne IPv4 služby
- [ ] HA vidí router v integraci Thread, OTBR integrace ukazuje dataset
- [ ] Matter zařízení jde commissionovat a ovládat
- [ ] Po odpojení napájení se síť obnoví (dataset z NVS) a RCP se znovu připojí
- [ ] Po restartu switche nebo routeru se obnoví Ethernet i Thread
- [ ] Stabilita: 7 dní bez restartu a výpadku zařízení
- [ ] Aktualizace RCP z P4 projde bez fyzického zásahu

## Výstupy projektu
- `firmware/rcp/` a `firmware/host/`: sdkconfig defaults, partition tabulka, případné patche
  nad esp-thread-br (submodul nebo pinned verze)
- `scripts/`: build, flash host, flash RCP přes JP1, backup C6
- `docs/`: schéma, foto zapojení propojek, postup obnovy továrního firmwaru C6
- README: provoz, aktualizace, troubleshooting

## Rizika a otevřené body

| Riziko | Dopad | Mitigace |
|---|---|---|
| SDIO linky P4↔C6 zůstanou nevyužité | kolize výstupů | v hostu i RCP nechat SDIO piny jako vstupy (nekonfigurovat) |
| Výkon antény C6 v modulu pro 802.15.4 | dosah | anténa modulu nebo konektor IPEX na modulu (je na fotce), změřit RSSI |
| REST API esp-thread-br je jen podmnožina ot-br-posix | některé funkce HA OTBR nemusí fungovat | otestovat hned v kroku 4; fallback je ot-br-posix s RCP na USB |
| Revize P4 (v1.x vs v3.x) | jiné nastavení IDF a targetu | ověřit podle boot logu před buildem |
| Nevratná ztráta továrního firmwaru C6 | deska už nepůjde použít jako BLE proxy | záloha v kroku 1.2; obnova je možná i z esp-hosted releasu |
| Žádný HA add-on ani automatické aktualizace | údržba | skripty a pinned verze v repu |

Hrubý odhad práce:
- 1–2 dny do routeru funkčního v HA;
- 1 den na testy Matteru a stability.

## Zdroje
- esp-thread-br: https://github.com/espressif/esp-thread-br, dokumentace https://docs.espressif.com/projects/esp-thread-br/en/latest/
- Podpora P4 jako hosta: https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md
- IDF `ot_br` a `ot_rcp`: https://github.com/espressif/esp-idf/tree/master/examples/openthread
- Koexistence C6: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html
- HA OTBR: https://www.home-assistant.io/integrations/otbr/, HA Thread: https://www.home-assistant.io/integrations/thread/
- Piny Ethernetu: https://devices.esphome.io/devices/Guition-ESP32-P4-M3-Dev
