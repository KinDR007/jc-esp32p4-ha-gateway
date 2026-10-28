# Report: UI test a oprava „HA automation“, 2026-10-28

Chyba (hlášená uživatelem): tlačítko „HA automation“ i „Copy TLVs“ končilo hláškou
`Cannot read properties of undefined (reading 'writeText')`. Brána běží na `http://`, kde prohlížeč
API schránky (`navigator.clipboard`) neposkytuje.

Oprava: šablona nebo dataset se zobrazí v dialogu. Kopíruje se přes schránku, pokud je dostupná,
jinak přes `execCommand('copy')`; text jde vždy i ručně označit.

| Běh | Výsledek |
|---|---|
| UI test se stránkou před opravou (`--html`) | UI02 FAIL, UI05 FAIL (stejná chyba jako u uživatele), ostatní PASS |
| UI test s opravenou stránkou na desce | UI01–UI05 PASS ([ui_test.log](ui_test.log)) |
