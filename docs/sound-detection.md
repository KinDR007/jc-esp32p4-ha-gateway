# Detekce zvukových událostí mikrofonem desky (analýza, 2026-10-28)

Stav: fáze 1 (audio základ) a 2 (YAMNet na zařízení) hotové a ověřené na desce, viz
[Výsledky fáze 1–2](#výsledky-fáze-12-2026-10-28). Fáze 3–5 čekají na potvrzení.

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
