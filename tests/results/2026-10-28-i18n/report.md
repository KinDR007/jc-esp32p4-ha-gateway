# Report: překlad web UI a přepínač vzhledu, 2026-10-28

Web UI je napsané anglicky; v češtině texty překládá vrstva ve stránce (slovník přesných řetězců a
pravidla pro řetězce s proměnnou částí). Návod pro Home Assistant má vlastní českou šablonu. Jména
zvuků (YAMNet), názvy událostí a technické hodnoty zůstávají v originále. Jazyk se volí podle
prohlížeče, vzhled podle systému; obojí jde přepnout v hlavičce a prohlížeč si volbu pamatuje.

| Běh | Výsledek |
|---|---|
| UI test se stránkou z v1.1 (`--html`) | UI06 FAIL, UI07 FAIL (přepínače chybí), UI01–UI05 PASS |
| UI test s novou stránkou na desce (OTA) | UI01–UI07 PASS ([ui_test.log](ui_test.log)) |
| Kontrola pokrytí (všechny záložky v češtině) | nepřeložené zůstávají jen hodnoty: adresy, verze, názvy sítí, čítače OpenThread |
