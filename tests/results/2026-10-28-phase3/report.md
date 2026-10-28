# Report: zvuk fáze 3 (události, personalizace), 2026-10-28

| Test | Výsledek | Vyjádření |
|---|---|---|
| TC14 Zvuková služba | PASS | inference 61 ms průměr, 123 ms max, CPU jádra 1 13 % |
| TC15 Rozpoznání | PASS | Speech 1,00; Dog 0,85, Bark 0,73 |
| TC17 Konfigurace událostí | PASS | validace, restart, předvolba, obnovení původní konfigurace |
| TC18 Událost z přehraného zvuku | PASS | `cat` vznikne; při ignorovaných třídách kočky nevznikne |

Ruční test 12 klipů ESC-50 viz [docs/sound-detection.md](../../../docs/sound-detection.md#výsledky-fáze-3-2026-10-28).
Strana Home Assistant (entity, události) neověřena: broker vyžaduje přihlášení.
