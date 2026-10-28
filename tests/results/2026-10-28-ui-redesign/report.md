# Report: přestavba web UI a HW informace, 2026-10-28

Levé menu s podsekcemi (na mobilu výsuvné), adresa stránky v URL, ikony MDI vložené jako SVG,
vysvětlivky ⓘ u údajů, zařízení v síti, rolí a akcí. Podrobné údaje o P4 a C6.

| Kontrola | Výsledek |
|---|---|
| UI testy UI01–UI08 na desce (OTA) | PASS ([ui_test.log](ui_test.log)) |
| `/api/status` → `hw` | CPU 2 × 360 MHz, zatížení jádro 0 ≈ 1 %, jádro 1 ≈ 18 % (zvuk), teplota 34,2 °C (34,2–35,2), 19 úloh, NVS 230/2520 |
| `c6.chip` (MAC, revize, flash C6) | zatím prázdné: čte se z bootloaderu C6 při příštím flashování nebo záloze, neověřeno |
| Teplota C6 | nedostupná: firmwary rádia (OpenThread RCP, ZBOSS NCP, esp-hosted) ji neposílají |
| Snímky desktop / mobil, světlý / tmavý | menu, dlaždice a ikony bez překryvů; na telefonu se skryje štítek role v hlavičce |
