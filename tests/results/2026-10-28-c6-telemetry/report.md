# Report: telemetrie C6 a opravy, 2026-10-28

Telemetrie C6 (teplota, volná RAM, doba běhu, důvod resetu) ve všech rolích brány, každá role
po vlastní lince. Výsledky hardwarových testů: [hw_test.log](hw_test.log), [hw_test.json](hw_test.json).

## Průběh a nalezené chyby

| # | Chyba | Projev | Příčina (jak zjištěno) | Oprava |
|---|---|---|---|---|
| 1 | Pád RCP p4gw.1/p4gw.2 | RCP padal (RESET_FAULT); P4 ho obnovovala a znovu posílala dotaz → brána nedostupná; p4gw.2 mrtvý hned po startu | backtrace z ladicího RCP v roli routeru (konzole 460800 → log P4): `nano-vfprintf` v `snprintf`, RCP linkuje newlib nano bez `%llu`, čte špatné argumenty | formát bez `%llu` a float, výpočet v samostatné úloze, obsluha Spinel jen kopíruje hotový text |
| 2 | Nekonečné přeflashování C6 (počet flashování > 30) | pojistka startů ukazovala stále 1/4 | restart po výpadku RCP je softwarový a pojistka ho brala jako vyžádaný | `gw_restart_on_failure()`: restart po selhání se počítá jako nestabilní start → po 4 režim údržby |
| 3 | Telemetrie RCP nedorazila | žádná data, žádná chyba | `RadioSpinel::Get` rozbaluje DATA do bufferu s kapacitou; předán ukazatel a kapacita 0 → EINVAL | kopie přímo do bufferu volajícího |
| 4 | Patch se po změně neaplikoval | brána sestavena se starým kódem | modul CMake běží jen při konfiguraci | `CMAKE_CONFIGURE_DEPENDS` na soubory patchů |
| 5 | Task watchdog při nahrávání firmwaru | reset během OTA se zapnutým rozpoznáváním zvuku | core dump: IDLE1 vyhladovělá, `httpd` (zděděná priorita 18) obsazoval jádro 1 mezi krátkými yieldy zvuku | web server přišpendlen na jádro 0 |
| 6 | Telemetrie koordinátoru chyběla | žádné rámce 0x7e | `app::start()` se nevrací (smyčka událostí), spuštění úlohy za ním se neprovedlo | spuštění před `app::start()` |
| 7 | TC06/TC08 občas selhaly | „OpenThread not running“ hned po přepnutí | test pokračoval po doflashování C6, OpenThread startuje o chvíli později; flasher navíc rušil příznak „busy“ před zápisem údajů čipu | test čeká na běžící OpenThread, flasher ruší „busy“ až nakonec |

Ochrana během ladění: smyčku jsme dvakrát zastavili zápisem balíku v1.1 přes USB (oddíl `c6fw`);
P4 si s novou pojistkou při zaseknutém dotazu na telemetrii (3 s) telemetrii pro danou verzi RCP vypne a restartuje se.

## Výsledky (deska, OTA)

| Test | Výsledek |
|---|---|
| TC04, TC05, TC06, TC07, TC08, TC13, TC16 (role a přechody) | PASS |
| TC19 telemetrie: Thread RCP 40,5 °C / 421 kB, bez výpadku RCP; Zigbee NCP 37,5 °C, host stream bez rámců 0x7e, NCP odpověděl na GET_MODULE_VERSION; Zigbee router 34,5 °C | PASS |
| Jednotkový test filtru `tests/unit/zb_filter_test.c` (všechna dělení proudu, DE/DE AD v datech, poškozené hlavičky, CRC, uříznutý rámec) | PASS; s rozbitým filtrem 2018 selhání |
| Kontrola TC19: podstrčený rámec 0x7e i vadné CRC parser odhalí | ano |
| Údaje o čipu C6 z bootloaderu | MAC 9c:13:9e:c4:04:d0, revize v0.2, flash 4 MB |
| Nahrání firmwaru se zapnutým rozpoznáváním zvuku po opravě jádra web serveru | bez watchdogu (1 nahrání, chyba je občasná) |

Neověřeno: pojistka startů a 3s pojistka telemetrie na reálném selhání (ověřeno revizí kódu); esp-hosted (ESPHome) telemetrii neposílá, web UI v té roli neběží.
