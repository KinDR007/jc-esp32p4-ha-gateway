# Report: zvuk fáze 1–2 a regrese se zapnutým zvukem (2026-10-28)

Deska JC-ESP32P4-M3-DEV (P4 v1.3), layout flash v2, gateway s komponentou `sound`, model YAMNet int8
(3,99 MB), mikrofon a rozpoznávání zapnuté po celou dobu regrese.

## Výsledek

| Test | Výsledek | Vyjádření |
|---|---|---|
| TC04 Thread BR | PASS | v cizí síti jako router (SRP/NAT64 u jiného BR), ve vlastní jako leader se SRP i NAT64 |
| TC05–TC07 Zigbee | PASS | flash C6 z komprimovaných balíků, snapshot Zigbee sítě obnoven |
| TC08 Thread router | PASS | |
| TC09 ESPHome a návrat | PASS | BLE 31 zařízení za 10 s, C6 zpět na ot_rcp |
| TC10 OTA | PASS | gateway image do 2MB slotu ESPHome odmítnut (413), slot nedotčen |
| TC11–TC13 | PASS | |
| TC14 Zvuková služba | PASS | model načten za 481 ms, inference 62 ms průměr / 109 ms max, CPU jádra 1 5,5 % |
| TC15 Rozpoznání (`--play`) | PASS | Speech 1,00; Dog 0,71, Bark 0,56 |
| TC16 Oddělené Thread sítě | PASS | border router se po roli router vrátil do své sítě |
| OTA upload při běžící inferenci | PASS | 18 s, bez resetu watchdogem (ručně, před TC14) |

První běh regrese (`hw_test.log`) měl TC04, TC08 a TC10 FAIL kvůli předpokladům testů (vlastní síť,
velikost slotu); testy jsou opravené a znovu spuštěné (`hw_test_rerun.log`, `hw_test_tc16.log`).

## Nálezy

- **Vedlejší účinek:** první běh regrese přepnul do role Thread router s datasetem uloženým z dřívějších
  testů. Tím se změnil aktivní dataset sdílený s border routerem a brána opustila síť
  `MyHome284238473` z Home Assistant. Oprava ve firmwaru (vlastní dataset border routeru, TC16) je
  nasazená. Do sítě `MyHome…` je potřeba bránu vrátit ručně datasetem z HA.
- Další problémy nalezené během fáze 1–2 jsou v
  [docs/sound-detection.md](../../../docs/sound-detection.md#výsledky-fáze-12-2026-10-28).
