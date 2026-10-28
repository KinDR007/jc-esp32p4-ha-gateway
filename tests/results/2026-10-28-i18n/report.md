# Report: překlad web UI a přepínač vzhledu, 2026-10-28

Web UI je napsané anglicky; v češtině texty překládá vrstva ve stránce (slovník přesných řetězců a
pravidla pro řetězce s proměnnou částí). Návod pro Home Assistant má vlastní českou šablonu. Jména
zvuků (YAMNet), názvy událostí a technické hodnoty zůstávají v originále. Jazyk se volí podle
prohlížeče, vzhled podle systému; obojí jde přepnout v hlavičce a prohlížeč si volbu pamatuje.

| Běh | Výsledek |
|---|---|
| UI test se stránkou z v1.1 (`--html`) | UI06 FAIL, UI07 FAIL (přepínače chybí), UI01–UI05 PASS |
| UI test s novou stránkou na desce (OTA) | UI01–UI07 PASS ([ui_test.log](ui_test.log)) |
| Český název události (`name_cs`) přes API na desce | uložená konfigurace z v1.1 dostala názvy z předvolby (19/19); prázdný `name_cs` ani přejmenovaná událost se nedoplní; vlastní název se uloží; konfigurace obnovena |
| „Právě slyším“ v češtině | třída patřící do události ukazuje český název události; test na stránce před opravou FAIL, po ní PASS |
| Atribut `name_cs` v HA | neověřeno, MQTT broker vyžaduje přihlášení |
| Kontrola pokrytí (všechny záložky v češtině) | nepřeložené zůstávají jen hodnoty: adresy, verze, názvy sítí, čítače OpenThread |

## Události tleskání a luskání

Do předvolby Domácnost a do aktuální konfigurace desky přibyly `clap` (Tlesknutí), `clapping`
(Opakované tleskání, aspoň 3 okna po 0,48 s) a `finger_snap` (Luskání prsty).

| Kontrola | Výsledek |
|---|---|
| YAMNet (PyTorch) na nahrávkách tleskání z ESC-50 | Clapping 0,99–1,00 v nejsilnějším okně |
| Tytéž nahrávky přehrané reproduktorem Macu k desce | nerozpoznáno (Typewriter, Coin), reproduktor zvuk tlesknutí nepřenese věrně |
| Luskání | bez testovací nahrávky; syntetický klik model nepozná, nutné ověřit živě |
| UI testy UI01–UI07 s 22 událostmi | PASS |
