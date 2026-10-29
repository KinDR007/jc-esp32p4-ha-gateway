# Detekce zvukových událostí mikrofonem desky (analýza, 2026-10-28)

Stav: fáze 1 (audio základ) a 2 (YAMNet na zařízení) hotové a ověřené na desce, viz
[Výsledky fáze 1–2](#výsledky-fáze-12-2026-10-28). Fáze 3 (personalizace a události v HA) je implementovaná a otestovaná na desce
([výsledky](#výsledky-fáze-3-2026-10-28)).
Seznam všech detekovatelných zvuků: [sound-classes.md](sound-classes.md).

## Cíl

Detekovat řeč, štěkot, zvonek a další zvuky v domácnosti mikrofonem desky. Události posílat přes
MQTT do Home Assistant. Výběr detekcí a učení vlastních zvuků řešit v nové záložce web UI. Stávající
role (Thread, Zigbee, ESPHome) musí fungovat beze změny.

## Hardware (ověřeno ze schémat)

| Signál | P4 GPIO | Poznámka |
|---|---|---|
| I2S MCLK / SCLK / LRCK | 13 / 12 / 10 | |
| I2S data z mikrofonu (ES8311 ASDOUT) | 48 | síť ve schématu se jmenuje `ES7210_SDOUT`, ES7210 na desce není |
| I2S data do reproduktoru (DSDIN) | 9 | |
| I2C SDA / SCL (ES8311, adresa 0x18) | 7 / 8 | |
| Zesilovač reproduktoru | 11 (PA_CTRL) | pro detekci není potřeba |

- Mikrofon je analogový, jde do ES8311 (MIC1P/N). Je to jeden kanál.
- ES8311 umí 16 kHz i 32 kHz.
- Žádný pin nekoliduje s Ethernetem ani s linkou na C6 (GPIO4/5/20/54).
- Zdroj: [schematics/3_8311_TFCARD.png](schematics/3_8311_TFCARD.png), [schematics/4_ESP32P4.png](schematics/4_ESP32P4.png).

## Zjištění

### Modely

| Model | Velikost int8 | Výpočet | Třídy | Stav na ESP | Licence |
|---|---|---|---|---|---|
| YAMNet | ~4 MB | 69 M MAC / 0,96 s | AudioSet 521 | int8 port pro TFLM existuje, na S3 ~500 ms/rámec [3] | Apache-2.0 |
| EfficientAT mn04_as | ~1 MB | 0,11 G MAC (délka klipu neuvedena) | AudioSet 527, mAP 43,2 vs. 30,6 YAMNet | port neexistuje, export ONNX → ESP-PPQ neověřen | MIT |
| Vlastní malá CNN (distilace) | 50–500 kB | jednotky ms | jen natrénované třídy | běžná cesta | vlastní |

- **Všechny požadované třídy YAMNet pokrývá:**
  - řeč (0–2)
  - pes a štěkot (69–75)
  - dveře, zvonek a ding-dong (348–350), klepání (353)
  - alarmy a kouřový hlásič (382, 390, 393, 394)
  - sklo (435–437)
  - pláč dítěte (19–20)
- **Frigate používá YAMNet se stejnými třídami.** Jeho mapování tříd a prahy jsou hotová šablona: práh 0,8 a model se spouští, jen když RMS ≥ `min_volume`.
- **EfficientAT je menší a přesnější.** U AudioSet modelů se ale předpokládá 32 kHz vstup (není potvrzeno). Nikdo ho na ESP neportoval. Je to tedy riziko, které se musí změřit.

### Runtime na ESP32-P4

- **ESP-DL v3 (MIT):** kvantizace přes ESP-PPQ, všechny potřebné operátory podporuje. MobileNetV2 int8 na P4 @400 MHz trvá 165 ms na jednom jádře.
  - Odhad pro YAMNet: 60–120 ms na inferenci, tedy **12–25 % jednoho jádra** při kroku 0,48 s.
  - Je to přepočet, ne měření.
- **TFLite Micro + esp-nn:** optimalizace pro P4 je zatím rozpracovaná. Odhad pro YAMNet je 300–500 ms na rámec, tedy skoro celé jádro. Nedoporučuji.
- **ESP-SR:** umí VAD a potlačení šumu, ale ne klasifikaci zvuků. Licence ho omezuje na čipy Espressif, což tady nevadí.

### Učení vlastních zvuků (few-shot)

- **Doporučený postup:**
  - Z nahraných vzorků spočítat embedding modelu (u YAMNet 1024 hodnot).
  - Uložit průměr vzorků (centroid).
  - Detekovat podle kosinové podobnosti.
- **Přesnost (studie na 24 domácích zvucích):** 80 % při 1 vzorku a 95 % při 4 vzorcích.
  - Když přijdou neznámé zvuky, klesne na 57–64 %.
  - Odmítání neznámých zvuků je proto hlavní problém.
- **Řešení odmítání:** 5–10 vzorků na profil a povinná nahrávka pozadí domácnosti. Práh se nastaví tak, aby na pozadí nevznikaly falešné detekce. K tomu požadavek na několik zásahů za sebou.
- **Na zařízení se ukládá jen embedding.** Zvuk zůstane v RAM jen po dobu nahrávání.

### Klasická DSP (náhrada nebo doplněk)

- **Goertzel nebo FFT s detekcí tónu a jeho časového vzoru:** levný a spolehlivý pro pípnutí pračky, piezo alarmy a dvoutónové gongy. Je to rozumný typ profilu „tónový signál“ v učicím režimu.
- **MFCC + DTW:** jde pro jeden konkrétní, stále stejný zvuk. Je citlivé na hluk a dozvuk.
- **Srovnávání čistého FFT spektra je nejslabší varianta.** Na řeč a štěkot nefunguje.

### Home Assistant

- **Doporučená je MQTT entita `event`.** Payload vypadá takto: `{"event_type": "dog_bark", "score": 0.91}`.
  - Zvonek bude samostatná entita s `device_class: doorbell`.
  - Nové naučené profily se do HA dostanou opětovným publikováním discovery.
- **Volitelně `binary_sensor` „řeč probíhá“** s `off_delay`.
- **Využije se stávající MQTT modul brány** (`p4gw/<host>/…`).

## Návrh architektury

- **Nová služba v gateway firmwaru**, nezávislá na roli. Ve výchozím stavu je **vypnutá**.
  - Běží ve rolích Thread a Zigbee.
  - V roli ESPHome ne, protože tam běží jiný image.
- **Řetězec zpracování:**
  - I2S 16 kHz mono
  - kruhový buffer v PSRAM
  - RMS brána: model se spouští jen při zvuku
  - log-mel
  - inference na jádře 1 s nízkou prioritou
  - třídy: prahy a vyhlazení přes N rámců
  - profily: embedding a kosinová podobnost, nebo tónový detektor
  - výstup do MQTT event a do web UI
- **Oddělení od síťových funkcí:**
  - Zigbee most a OpenThread se připnou na jádro 0.
  - Audio dostane pevný rozpočet CPU.
  - Hlídací pes vypne detekci, když inference zabere víc než například 40 % jádra nebo když klesne volná interní RAM.
  - Regrese TC04–TC13 se spustí znovu se zapnutou detekcí.
- **Uložení modelu:**
  - EfficientAT (~1 MB) se vejde do stávajícího oddílu `c6fw` jako další položka. Ten má 2,2 MB volno, takže **tabulka oddílů se nemění**.
  - YAMNet (~4 MB) se nevejde ani do slotu aplikace, ani do `c6fw`. Vyžaduje **změnu tabulky oddílů**: například zmenšit slot ESPHome ze 4 na 2 MB a vytvořit oddíl `models`. To znamená jednorázový flash přes USB, přenos Zigbee snapshotů a nové nahrání `c6fw`. NVS a Thread síť zůstanou zachované.
- **Měření před integrací:** samostatná měřicí aplikace se nahraje do slotu ESPHome. Gateway ji spustí stejně jako ESPHome a aplikace se po měření sama vrátí do gateway. Měří se tak na reálném hardwaru bez zásahu do gateway firmwaru.
- **Web UI, záložka „Sound“:**
  - zapnutí a vypnutí, výběr tříd a prahů
  - živá hladina zvuku a poslední detekce
  - učicí režim: nahrát 5–10 vzorků a pozadí, otestovat, uložit, smazat
  - viditelný indikátor nahrávání

## Rizika

| Riziko | Dopad | Mitigace |
|---|---|---|
| Inference zatíží CPU a zpomalí Zigbee/Thread | latence sítě | jádro 1, nízká priorita, RMS brána, limit CPU, regresní testy |
| Kvalita int8 kvantizace (YAMNet port kalibrovaný na syntetických datech) | horší přesnost u vzácných tříd | vlastní kalibrace ESP-PPQ na reálných nahrávkách |
| EfficientAT bez portu a s nejasnou vzorkovací frekvencí | zdržení | rozhodne měřicí aplikace; YAMNet jako ověřená záloha |
| Falešné poplachy u naučených profilů | spam v HA | nahrávka pozadí, kalibrovaný práh, N po sobě, cooldown |
| Soukromí (mikrofon v domácnosti) | důvěra | jen na zařízení, ven odchází jen štítek a skóre, vypnuto ve výchozím stavu, heslo v UI |
| Kvalita mikrofonu a citlivost ES8311 | dosah detekce | ověřit na desce v první fázi (hladina, šum) |

## Navržené fáze (každá s testem na desce a regresí stávajících rolí)

1. **Audio základ:**
   - ovladač ES8311 a I2S, měření hladiny a šumu
   - MQTT senzor hladiny a záložka Sound s živou hladinou
   - vypnuté ve výchozím stavu
   - regrese TC04–TC13
2. **Měření modelů:** měřicí aplikace ve slotu ESPHome změří YAMNet a EfficientAT přes ESP-DL na P4 (latence, paměť, kvalita na reálných nahrávkách). Podle výsledku se vybere model.
3. **Detekce tříd:** inference, mapování tříd podle Frigate, MQTT event entity, výběr detekcí v UI, nové testovací případy.
4. **Učicí režim:** profily embedding a centroid, profily typu tónový signál (Goertzel), nahrávka pozadí, kalibrace prahu.
5. **Ladění a stabilita:** falešné poplachy, 24h test se zapnutou detekcí v roli Thread BR i Zigbee.

## Výsledky fáze 1–2 (2026-10-28)

Rozhodnutí uživatele: model YAMNet, detekovat vše co model umí, učicí režim bude uchovávat vzorky.

### Implementace

- Komponenta `firmware/gateway/components/sound`:
  - ES8311 přes `esp_codec_dev`, I2S 16 kHz, mikrofon na levém slotu
  - kruhový buffer 10 s v PSRAM, hladina zvuku a šumové pozadí
  - log-mel v C (`sound_mel.c`), ověřený proti Python referenci s odchylkou max 5e-5 (`tools/yamnet/test_mel.py`)
  - inference přes ESP-DL 3.3, parametry modelu se kopírují do PSRAM
  - ve výchozím stavu vypnuto
  - zachytávání a inference běží na jádře 1 s nízkou prioritou; inference se spouští jen nad prahem hlasitosti
  - pojistka CPU vypne inferenci nad 60 % jádra
  - zastaralý zvuk se zahazuje, nedohání se
- Model `tools/yamnet`:
  - YAMNet přepsaný do PyTorch s oficiálními váhami (3,74 M vah, 69,2 M MAC na patch)
  - kvantizace ESP-PPQ int8 pro P4, kalibrace na ESC-50
  - logity oříznuté na ±16, jinak by měly kvantizační krok 4
  - shoda s float modelem: top-1 79 %, top-1 v top-5 97 %
  - build: `scripts/build_model.sh`
- ESP-DL se kompiluje jen s operátory a int8 jádry, která model používá (`yamnet.compile_req.yml`).
  Bez toho by měla binárka 3,46 MB a do 3MB slotu by se nevešla; takhle má 2,35 MB.
- Rozdělení flash v2:
  - oddíly `model` (4 MB) a `sounds` (1,56 MB), slot ESPHome 2 MB
  - balíky C6 komprimované (2,77 → 1,65 MB); P4 je při flashování rozbalí
  - NVS se nemění: role, Thread síť i nastavení migraci přežijí
- Web UI, záložka Sound: zapnutí, zesílení, práh, živá hladina, nahrávka 5 s, upload modelu,
  statistiky inference, top 5 tříd.
- MQTT: senzor `Sound level` (dBFS), když je mikrofon zapnutý.

### Měření na desce (P4 v1.3, 360 MHz)

| Veličina | Hodnota |
|---|---|
| Načtení modelu (4 MB do PSRAM) | 407–481 ms |
| Inference na patch (0,96 s zvuku, každých 0,48 s) | 59–62 ms průměr, max 109 ms |
| Zátěž jádra 1 | 5–16 % (inference běží jen nad prahem hlasitosti) |
| Řeč (syntéza `say` z reproduktoru Macu) | Speech 1,00 |
| Štěkot (ESC-50) | Dog 0,71–0,82, Bark 0,56–0,68 |
| Regrese rolí se zapnutým zvukem | TC04–TC13 PASS (po úpravě testů), OTA upload při běžící inferenci bez resetu |

### Problémy nalezené na desce a opravy

| Problém | Oprava |
|---|---|
| Watchdog při dlouhém OTA uploadu se zapnutým zvukem (IDLE1 nedostal čas) | upload i výpočet mel pravidelně uvolní jádro, zpožděný zvuk se zahazuje |
| Skóre jen 0,02 nebo 0,50 | logity oříznuté na ±16 před kvantizací |
| Komprimovaný zápis do C6 přes jeho ROM loader dával špatná data | rozbalení na P4 (tinfl na haldě, kontrola MD5), zápis nekomprimovaně |
| Restart z UI v první minutě po aktualizaci vrátil starou verzi | restart vyžádaný přes API nejdřív potvrdí běžící verzi |
| Role Thread router přepsala síť border routeru | border router si ukládá vlastní dataset (`br_dataset`), test TC16 |

## Fáze 3: personalizace detekcí a předávání do HA

### Výchozí stav

- Model hodnotí vždy všech 521 tříd najednou. Seznam je v [sound-classes.md](sound-classes.md).
  Tříd je hodně a nejsou všechny užitečné: 145 je hudba (žánry, nástroje) a 21 popisuje akustické
  prostředí („Inside, small room“, „Silence“), takže nejde o události.
- Model nepozná, odkud zvuk pochází. Řeč z televize je pro něj také „Speech“ a letadlo v reklamě je
  „Aircraft“. Personalizace tedy musí řešit dvě věci: co mě vůbec zajímá a co je v tomto bytě šum.

### Dvouúrovňový model: třídy → události

1. **Třídy (521, úroveň modelu):** v UI jako strom podle ontologie AudioSet, tedy 7 kategorií a asi
   40 podskupin. U každé položky je zaškrtávátko, fulltextové hledání a živé skóre („co právě
   slyším“). Odškrtnutá třída se nikdy nehlásí, např. letadla, hudební žánry nebo „Inside, small room“.
2. **Události (desítky, úroveň HA):** pojmenovaná skupina tříd s vlastním prahem a časováním.
   Do Home Assistant jdou jen události, nikdy 521 surových tříd. Příklad: `dog_bark` = Dog, Bark, Yip,
   Howl, Bow-wow, Growling, Whimper (dog). Událost má:
   - `id` (stabilní, z něj vzniká `unique_id` v HA; přejmenování nerozbije automatizace)
   - název
   - třídy
   - práh skóre
   - minimální trvání: kolik oken po 0,48 s za sebou musí práh překročit
   - dobu doznění: jak dlouho po posledním zásahu událost trvá
   - cooldown: minimální odstup dvou událostí
   - druh: „impulzní“ (zvonek, sklo, štěkot) nebo „trvalá“ (řeč, voda teče, vysavač)
3. **Předvolby** (jedním klikem, dál upravitelné):

| Předvolba | Události |
|---|---|
| Domácnost (výchozí) | speech, shout_scream, laughter, baby_cry, cough_sneeze, snoring, dog_bark, cat, doorbell, knock, door, glass_break, smoke_alarm, alarm_siren, phone_ring, water_running, toilet_flush, vacuum, appliance (mikrovlnka, mixér, fén), clap (tlesknutí), clapping (opakované tleskání, aspoň 3 okna po sobě), finger_snap (luskání prsty) |
| Bezpečnost | glass_break, smoke_alarm, alarm_siren, shout_scream, door, knock, dog_bark, gunshot_explosion |
| Zvířata | dog_bark, dog_whimper, cat, bird |
| Vše | jedna událost na každou třídu kromě hudby a prostředí (pro ladění) |

4. **Výchozí vyloučení:** kategorie „Channel, environment and background“, „Silence“, hudební žánry a
   většina „Onomatopoeia“ jsou ve výchozím stavu odškrtnuté.
5. **Ladění z provozu:** UI ukáže log posledních detekcí, tedy čas, událost, třídu, skóre a hlasitost.
   U každého záznamu půjde jedním klikem „tuto třídu ignorovat“ nebo „zvýšit práh“. Tak se
   personalizace dělá podle toho, co skutečně zní v bytě, ne podle odhadu předem.
6. **Potlačení zdrojů (volitelné):** pravidlo „ignoruj `speech`, když je současně Television, Radio
   nebo Music nad prahem“ omezí falešnou řeč z TV. Úplné řešení to není, YAMNet TV od člověka spolehlivě
   nerozliší.

Konfigurace se uloží jako JSON v NVS (desítky událostí se vejdou do jednotek kB). V UI bude export a
import. Naučené zvuky z fáze 4 budou jen další události stejného tvaru.

### Detekční logika (na zařízení)

- Skóre události je maximum skóre jejích povolených tříd v daném okně.
- **Začátek:** skóre ≥ práh v N oknech za sebou. Pro impulzní zvuky stačí 1, pro trvalé 2–3 okna
  (zhruba 1–1,5 s).
- **Konec:** po dobu doznění žádné okno nad `práh − 0,1` (hystereze).
- **Cooldown:** po skončení se událost stejného typu hlásí nejdřív za X s, např. zvonek 30 s.
  Brání to záplavě notifikací při štěkání.
- **Výchozí prahy:** kalibrace na desce ukázala čisté zvuky v rozsahu 0,6–1,0 a ticho pod 0,3.
  Výchozí práh bude 0,5 a v UI půjde nastavit. Frigate používá 0,8, ale na float modelu.

### Předávání do Home Assistant: varianty

| Mechanismus (MQTT discovery) | Co v HA vznikne | Vhodné pro | Omezení |
|---|---|---|---|
| **`event` entita** | `event.p4gw_dog_bark`, stav = čas poslední události, atributy (třída, skóre, hlasitost) | impulzní zvuky, historie v logbooku, automatizace „když nastane“ | žádný stav „probíhá“ |
| **`binary_sensor`** s doběhem | `binary_sensor.p4gw_speech` on/off | trvalé zvuky, podmínky („když běží vysavač“), triggery s `for:` („štěká déle než 2 min“), dashboardy | u krátkých zvuků jen krátký puls |
| **`device_trigger`** | trigger „P4 gateway: Dog bark detected“ v editoru automatizací (Zařízení → spouštěč) | nejpohodlnější tvorba automatizací v UI | bez entity a bez historie |
| `sensor` „poslední zvuk“ | text + atributy | přehled na dashboardu | pro automatizace nevhodný |
| vlastní integrace HA | cokoliv | – | zbytečná práce, MQTT stačí |

**Doporučení:** pro každou povolenou událost publikovat **`event` entitu + `binary_sensor` +
`device_trigger`** z jedné discovery. Uživatel v HA použije, co se mu hodí, a nepotřebné entity
v HA vypne.
- Payload na `p4gw/<host>/sound/<event_id>`:
  `{"event_type": "detected", "class": "Bark", "score": 0.82, "level_db": -41.7, "duration_s": 2.4}`
- Stav `binary_sensor`: `p4gw/<host>/sound/<event_id>/state` = ON/OFF
- Při přidání nebo odebrání události se discovery publikuje znovu. Odebraná událost pošle prázdnou
  retained konfiguraci, takže entita z HA zmizí.

### Příklady automatizací (HA)

```yaml
# Zvonek -> notifikace a TTS
trigger:
  - platform: state
    entity_id: event.p4gw_doorbell
action:
  - service: notify.mobile_app_telefon
    data: {message: "Zvoní zvonek"}

# Pes štěká déle než 2 minuty, nikdo není doma
trigger:
  - platform: state
    entity_id: binary_sensor.p4gw_dog_bark
    to: "on"
    for: "00:02:00"
condition:
  - condition: state
    entity_id: zone.home
    state: "0"
action:
  - service: notify.mobile_app_telefon
    data: {message: "Pes štěká už 2 minuty"}

# Kouřový hlásič -> kritická notifikace, rozbití skla při zapnutém alarmu -> siréna
trigger:
  - platform: state
    entity_id: event.p4gw_smoke_alarm
  - platform: state
    entity_id: event.p4gw_glass_break
    id: glass
condition:
  - condition: or
    conditions:
      - condition: template
        value_template: "{{ trigger.id != 'glass' }}"
      - condition: state
        entity_id: alarm_control_panel.dum
        state: armed_away
action:
  - service: notify.mobile_app_telefon
    data: {message: "{{ trigger.to_state.attributes.class }} ({{ trigger.to_state.attributes.score }})"}
```

Plán: v UI brány u každé události tlačítko „zkopírovat šablonu automatizace“.

### Výsledky fáze 3 (2026-10-28)

Implementováno podle návrhu výše:
- Firmware `sound_events.c`:
  - konfigurace v NVS, předvolby `household`, `security`, `animals` a `minimal` (`tools/yamnet/gen_presets.py`)
  - detekce s okny, hysterezí, doběhem, cooldownem a potlačením
  - log 64 detekcí včetně tříd bez události
  - pro každou povolenou událost MQTT entity `event`, `binary_sensor` (třída `sound`) a `device_trigger`
  - smazaná nebo vypnutá událost se z HA odebere
- API:
  - `GET/PUT /api/sound/events`, `POST /api/sound/events/preset`
  - `GET /api/sound/classes`, `/api/sound/scores`, `/api/sound/log`
- UI, záložka Sound:
  - tabulka událostí (zapnutí, editor, smazání, šablona automatizace HA, export a import JSON)
  - log detekcí s akcemi „Ignore sound“, „Raise threshold“ a „Create event“
  - strom 521 zvuků s vyhledáváním a živým skóre
- Entity v HA, `<host>` je hostname s `_` místo `-`:
  - `event.<host>_snd_<id>` (atributy `event_type`, `class`, `score`, `level_db`, `name_cs`)
  - `name_cs`: volitelný český název události. Web UI ho v češtině zobrazuje místo anglického názvu, do HA jde
    jen jako atribut; ID, názvy entit a automatizace zůstávají anglicky. Předvolby ho mají vyplněný, starší
    uložené události ho převezmou z předvolby se stejným ID a anglickým názvem.
  - `binary_sensor.<host>_snd_<id>_active`
  - device trigger „sound <název>“

Test přehráváním klipů ESC-50 z reproduktoru Macu vedle desky:

| Zvuk | Výsledek |
|---|---|
| zaklepání, splachování, kočka | správná událost (`knock` 0,53, `toilet_flush` 0,99, `cat` 0,98) |
| budík | `phone_ring` a `doorbell` (podobný zvuk) |
| vysavač | v logu bez události jako Aircraft/Helicopter 0,9: typický případ pro „Ignore sound“ |
| rozbité sklo | nepoznáno (přes reproduktor notebooku Rub/Filing) |
| pláč, kýchnutí | falešně `speech`; pro řeč je vhodné zvýšit práh nebo přidat potlačení |

Přehrávání z reproduktoru notebooku zkresluje. Reálné zvuky v bytě je potřeba doladit podle logu
(fáze 5). Automatické testy TC14, TC15, TC17 a TC18 prošly
([výsledky](../tests/results/2026-10-28-phase3/)). Stranu Home Assistant (entity a události
v HA) je potřeba ověřit v HA, protože broker 192.168.88.27 vyžaduje přihlášení.

### Rozsah implementace fáze 3

- Firmware:
  - modul událostí: konfigurace, detekční logika a MQTT discovery pro event, binary_sensor a device_trigger
  - API `GET/PUT /api/sound/events`, `GET /api/sound/classes` (strom s živými skóre), `GET /api/sound/log`
- UI, záložka Sound:
  - strom tříd s vyhledáváním a předvolbami
  - editor událostí
  - log detekcí s rychlými akcemi
  - šablony automatizací
- Testy:
  - TC17: konfigurace událostí přežije restart a odškrtnutá třída se nehlásí
  - TC18: discovery v MQTT a payloady událostí (kontrola přes broker)
  - TC19: přehraný zvuk vyvolá událost v HA
- Odhad: 1 den práce a test na desce s brokerem 192.168.88.27.

## Fáze 4: naučené zvuky (2026-10-29)

Implementace (`firmware/gateway/components/sound/sound_learn.c`, UI Zvuk › Naučené zvuky):
- **Vzorek:** 0,96 s zvuku (jedno okno modelu, 15 600 vzorků) a jeho embedding YAMNet (1024 hodnot).
  Ukládá se do oddílu `sounds` po slotech 36 kB, celkem 44 slotů. Zvuk zůstává uložený, takže jde
  přehrát a profil se dá kdykoli přepočítat.
- **Zdroje vzorků:**
  - z mikrofonu: tlačítko Nahrát, uloží se nejhlasitější okno do 4 s; ticho pod prahem spouštění se odmítne
  - ze souboru WAV (16 kHz mono, 1–10 s), např. nahrávka telefonem přímo u spotřebiče
- **Profily** (vzorky se stejným názvem):
  - *jakýkoli zvuk:* kosinová podobnost embeddingu s průměrem vzorků (centroid)
  - *pípnutí nebo tón:* dominantní frekvence vzorků (Goertzel, hrubé hledání 40 Hz v nejhlasitějších
    4096 vzorcích, pak jemné po 5 Hz) a podíl energie na ní, poměřený se vzorky
- **Pozadí:** 5 nahrávek místnosti (mikrofon 5 s nebo WAV). Kalibrace:
  - shoda vzorků = nejnižší skóre vzorku proti ostatním (leave-one-out)
  - pozadí = nejvyšší skóre nahrávek pozadí
  - navržený práh = pozadí + 70 % rozdílu ke shodě vzorků, s minimem 0,55 pro embedding
    (embeddingy jsou nezáporné, nesouvisející zvuky dávají podobnost 0,45–0,53) a 0,3 pro tón;
    když se pozadí a vzorky překrývají, UI to označí
- **Událost** s polem `profile` místo `classes`: stejná detekční logika, MQTT entity a log (třída
  „learned sound“). Tlačítko Vytvořit událost ji založí s navrženým prahem.
- **Test nahrávky:** WAV se vyhodnotí proti všem profilům a nic se neuloží.
- **API:**
  - `GET/DELETE /api/sound/learn`
  - `POST /api/sound/learn/record` (`kind` sample/background, `profile`, `type` embedding/tone)
  - `POST /api/sound/learn/upload?kind=…&profile=…&type=…` nebo `?test=1` (tělo WAV)
  - `GET /api/sound/learn/sample?slot=n`

Ověření (TC28, nahrávání WAV):

| Profil | Shoda vzorků | Pozadí (místnost) | Práh | Pozitivní test | Negativní test |
|---|---|---|---|---|---|
| tón 2000 Hz (3 vzorky) | 0,85 | 0 | 0,60 | pípnutí 2010 Hz: 0,85 | tón 900 Hz: 0 |
| pes (2 klipy ESC-50) | 0,64 | 0,19 | 0,51 | jiný pes: 0,77 | déšť, tleskání, ohňostroj: max 0,46 |

S prostředním prahem (0,43) by ohňostroj (0,49) falešně spustil událost „pes“; proto 70 % a minimum 0,55.
Skóre téhož souboru se mezi běhy liší o 0,01–0,07 (cizí zvuky 0,45–0,53 ve třech bězích, shoda vzorků 0,61–0,64);
příčina zatím není známá (esp-dl běží na jednom jádře, výpočet melů je deterministický), viz TODO.
S 5–10 vzorky stejného zdroje bude shoda vzorků i rezerva vyšší. Živé nahrávání z mikrofonu a spuštění
události přehraným zvukem čeká na test s reproduktorem u desky (výstup Macu byl ztlumený).

## Hlučnost v dB(A) (2026-10-29)

Brána kromě hladiny v dBFS (digitální hladina vůči plnému rozsahu převodníku, 0 = nejhlasitější
možný signál) měří akustický tlak v dB(A), tedy totéž co hlukoměr nebo aplikace v telefonu.

- **Váhový filtr A** (IEC 61672) na každém vzorku 16 kHz: póly pod Nyquistovou frekvencí
  bilineární transformací, dvojpól 12,2 kHz (nad Nyquistem) nahrazen dvoukoeficientovým FIR.
  Odchylka od normy do 0,2 dB mezi 31,5 Hz a 7 kHz
  (`components/sound/sound_aweight.c`, test `tests/unit/aweight_test.c`).
- **Úrovně:** LAS (pomalá odezva, 1 s), LAeq (energetický průměr) a LAmax (nejvyšší 100ms
  úroveň) za okno 60 s. Do dokončení prvního okna se hlásí průběžné hodnoty (`partial`). První
  sekunda po startu mikrofonu se nepočítá (lupnutí kodeku). Přebuzení (vzorek ≥ 0,99 FS) se hlásí.
- **Převod:** `dB(A) = dBFS(A) + posun − zesílení`. Změna zesílení kalibraci zachová.
  Výchozí posun 135 dB je odhad pro typický analogový mikrofon (−42 dBV/Pa) do ES8311 (plný
  rozsah asi −1 dBV), chyba asi ±6 dB.
- **Kalibrace** (Zvuk › Živě › Hlučnost): hlukoměr nebo aplikace (vážení A, pomalá odezva) vedle
  brány v ustáleném hluku, zadat údaj → posun se dopočítá z aktuální LAS. API
  `POST /api/sound/config` s `spl_reference` (20–130 dB(A)), `spl_offset` (přímo) nebo
  `spl_reset`. Posun je v NVS (`snd_spl`) a jde i do zálohy nastavení.
- **Home Assistant:** senzory *Hluk* (LAeq) a *Hluk max* (LAmax), třída `sound_pressure`,
  jednotka dBA. Jsou v integraci P4 Gateway i v MQTT (`noise_laeq`, `noise_max` ve stavu).
  Hladina v dBFS přešla mezi diagnostické.
- **Meze:** nejtišší měřitelná hodnota je dána šumem mikrofonu a kodeku (v tiché místnosti
  odhadem 35–40 dB(A) při výchozím odhadu). Pásmo končí na 8 kHz. Jde o orientační měření, ne
  o certifikovaný hlukoměr. Test TC31.

## Zdroje

1. YAMNet: https://github.com/tensorflow/models/tree/master/research/audioset/yamnet, třídy: yamnet_class_map.csv
2. EfficientAT: https://github.com/fschmid56/EfficientAT
3. YAMNet int8 pro TFLM: https://huggingface.co/chayuto/yamnet-mel-int8-tflm
4. ESP-DL: https://github.com/espressif/esp-dl, benchmark: https://components.espressif.com/components/espressif/imagenet_cls
5. esp-tflite-micro / esp-nn: https://github.com/espressif/esp-tflite-micro, https://github.com/espressif/esp-nn
6. ESP-SR: https://github.com/espressif/esp-sr, https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/benchmark/README.html
7. Few-shot, domácí zvuky: https://arxiv.org/abs/2002.11561
8. Frigate audio: https://docs.frigate.video/configuration/audio_detectors/
9. Goertzel: https://mstarlabs.com/dsp/goertzel/goertzel.html
10. HA MQTT event: https://www.home-assistant.io/integrations/event.mqtt/
11. AudioSet licence (třídy CC BY-SA 4.0): https://research.google.com/audioset/download.html
