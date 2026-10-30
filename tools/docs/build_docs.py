#!/usr/bin/env python3
"""Builds the user documentation documentation/{en,cs}/index.html from one bilingual source.

The pages look like the web UI: same colours, header, side menu, cards and MDI icons (taken from
the ICONS block of firmware/gateway/web/index.html). Screenshots: documentation/screenshots, made by
the browser script described in documentation/README.md; index.json lists the cards of each page.

    python3 tools/docs/build_docs.py
"""

import html
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "documentation"
SHOTS = OUT / "screenshots"
UI = (ROOT / "firmware/gateway/web/index.html").read_text()
ICONS = dict(re.findall(r"^  '([a-z0-9-]+)': '([^']+)',?$", UI.split("/* ICONS:BEGIN */")[1].split("/* ICONS:END */")[0], re.M))
CARDS = json.loads((SHOTS / "index.json").read_text())
# card titles as the web UI writes them (h3 shows them upper-case) and their Czech translations
UI_TITLES = {t.upper(): t for t in re.findall(r"h3\('[a-z0-9-]+', (?:L\()?'([^']+)'", UI)}
UI_CS = dict(re.findall(r"'([^'\\]+)': '([^'\\]+)'", UI.split("const CS = {")[1].split("\n};")[0]))
UI_CS_TITLES = dict(re.findall(r"h3\('[a-z0-9-]+', L\('([^']+)', '([^']+)'\)", UI))


# cards whose titles the web UI builds at run time
OWN_TITLES = {
    "NEIGHBORS": ("Neighbors", "Sousedé"), "ROUTERS": ("Routers", "Routery"), "ALL SOUNDS": ("All sounds", "Všechny zvuky"),
    "P4 GATEWAY INTEGRATION": ("P4 Gateway integration", "Integrace P4 Gateway"),
    "THREAD BORDER ROUTER → HOME ASSISTANT": ("Thread border router → Home Assistant", "Thread border router → Home Assistant"),
    "ZIGBEE COORDINATOR → ZIGBEE2MQTT (RECOMMENDED)": ("Zigbee coordinator → Zigbee2MQTT (recommended)", "Zigbee koordinátor → Zigbee2MQTT (doporučeno)"),
    "ESPHOME BLE PROXY ROLE": ("ESPHome BLE proxy role", "Role ESPHome BLE proxy"), "ESPHOME IMAGE": ("ESPHome image", "Obraz ESPHome"),
}


def card_name(upper: str, lang: str) -> str:
    """Name of a card from its screenshot title (upper-case), in the language of the page."""
    key = re.sub(r"\s*\(\d+\)$", "", upper)
    if key in OWN_TITLES:
        return OWN_TITLES[key][lang == "cs"]
    en = UI_TITLES.get(key) or next((t for t in UI_CS if t.upper() == key), key.capitalize())
    if lang == "cs":
        return UI_CS_TITLES.get(en) or UI_CS.get(en) or en
    return en


def jpeg_size(path: Path):
    """(width, height) from the SOF marker of a JPEG."""
    data = path.read_bytes()
    i = 2
    while i < len(data):
        marker, length = data[i + 1], int.from_bytes(data[i + 2:i + 4], "big")
        if 0xC0 <= marker <= 0xC3:
            return int.from_bytes(data[i + 7:i + 9], "big"), int.from_bytes(data[i + 5:i + 7], "big")
        i += 2 + length
    return None


def ic(name: str, cls: str = "mdi") -> str:
    return f'<svg class="{cls}" viewBox="0 0 24 24" aria-hidden="true"><path d="{ICONS.get(name, ICONS["information-outline"])}"/></svg>'


# ---------------------------------------------------------------- content
# Each text is (english, czech). Strings may hold simple HTML; <code>, <b>, <a>.

def T(en, cs):
    return {"en": en, "cs": cs}


INTRO = T(
    """<p>The <b>P4 Gateway</b> turns the Guition JC-ESP32P4-M3-DEV board into a LAN gateway for Home Assistant. The ESP32-P4
    runs the gateway, the web UI and sound recognition; the ESP32-C6 in the module is the 2.4 GHz radio. One role runs at a
    time and is switched in the web UI, from Home Assistant or from Telegram.</p>""",
    """<p><b>P4 Gateway</b> dělá z desky Guition JC-ESP32P4-M3-DEV síťovou bránu pro Home Assistant. ESP32-P4 obsluhuje bránu,
    webové rozhraní a rozpoznávání zvuků; ESP32-C6 v modulu je rádio 2,4 GHz. Najednou běží jedna role, přepíná se ve
    webovém rozhraní, z Home Assistant nebo z Telegramu.</p>""")

ROLES_TABLE = T(
    """<table><thead><tr><th>Role</th><th>P4</th><th>C6</th><th>Home Assistant</th></tr></thead><tbody>
    <tr><td>Thread border router</td><td>OpenThread BR, REST on port 8081</td><td>ot_rcp</td><td>OpenThread Border Router + Thread integrations, Matter</td></tr>
    <tr><td>Thread router</td><td>joins an existing Thread network</td><td>ot_rcp</td><td>nothing, extends the mesh</td></tr>
    <tr><td>Zigbee coordinator</td><td>UART–TCP bridge, port 6638</td><td>ZBOSS NCP</td><td>Zigbee2MQTT, <code>adapter: zboss</code></td></tr>
    <tr><td>Zigbee router</td><td>monitor and commands</td><td>own router firmware</td><td>joins by permit join</td></tr>
    <tr><td>ESPHome BLE proxy</td><td>ESPHome image</td><td>esp-hosted</td><td>ESPHome integration</td></tr></tbody></table>
    <p>Independent of the role (except ESPHome): sound recognition with the onboard microphone (YAMNet, 521 sound classes),
    noise level in dB(A), notifications, user accounts and the Telegram bot.</p>""",
    """<table><thead><tr><th>Role</th><th>P4</th><th>C6</th><th>Home Assistant</th></tr></thead><tbody>
    <tr><td>Thread border router</td><td>OpenThread BR, REST na portu 8081</td><td>ot_rcp</td><td>integrace OpenThread Border Router a Thread, Matter</td></tr>
    <tr><td>Thread router</td><td>připojí se do existující sítě Thread</td><td>ot_rcp</td><td>nic, rozšiřuje síť</td></tr>
    <tr><td>Zigbee koordinátor</td><td>most UART–TCP, port 6638</td><td>ZBOSS NCP</td><td>Zigbee2MQTT, <code>adapter: zboss</code></td></tr>
    <tr><td>Zigbee router</td><td>dohled a příkazy</td><td>vlastní firmware routeru</td><td>připojí se přes permit join</td></tr>
    <tr><td>ESPHome BLE proxy</td><td>obraz ESPHome</td><td>esp-hosted</td><td>integrace ESPHome</td></tr></tbody></table>
    <p>Nezávisle na roli (kromě ESPHome): rozpoznávání zvuků vestavěným mikrofonem (YAMNet, 521 tříd zvuků), hlučnost
    v dB(A), notifikace, uživatelské účty a Telegram bot.</p>""")

HW = T(
    """<p>Module JC-ESP32P4-M3: ESP32-P4NRW32 (16 MB flash, 32 MB PSRAM) with an ESP32-C6. Inside the module the P4 and the C6
    share only SDIO (used by the ESPHome role) and the C6 reset. Thread and Zigbee need a UART, which exists only on header
    <b>JP1</b>: three jumper wires (female–female dupont) are needed.</p>
    <table><thead><tr><th>JP1 from</th><th>JP1 to</th><th>Purpose</th></tr></thead><tbody>
    <tr><td>13 (P4 GPIO4, TX)</td><td>20 (C6 U0RXD)</td><td>UART P4 → C6</td></tr>
    <tr><td>15 (P4 GPIO5, RX)</td><td>22 (C6 U0TXD)</td><td>UART C6 → P4</td></tr>
    <tr><td>17 (P4 GPIO20)</td><td>24 (C6 IO9 / BOOT)</td><td>the P4 flashes the C6 through its ROM bootloader</td></tr></tbody></table>
    <p class="note">Check pin 1 with a multimeter first (3.3 V against GND; pin 2 shows 5 V). Pin 26 stays free, the P4 drives
    the C6 reset inside the module. The ESPHome role works without jumpers.</p>
    <p>USB-C next to the Ethernet jack: CH340, console and flashing (opening the port resets the board). Ethernet IP101 at
    100 Mb/s; power over USB-C.</p>""",
    """<p>Modul JC-ESP32P4-M3: ESP32-P4NRW32 (16 MB flash, 32 MB PSRAM) s ESP32-C6. Uvnitř modulu jsou P4 a C6 propojené jen
    přes SDIO (používá role ESPHome) a reset C6. Thread a Zigbee potřebují UART, který je vyvedený jen na konektor
    <b>JP1</b>: jsou potřeba tři propojky (dupont samice–samice).</p>
    <table><thead><tr><th>JP1 z</th><th>JP1 do</th><th>Účel</th></tr></thead><tbody>
    <tr><td>13 (P4 GPIO4, TX)</td><td>20 (C6 U0RXD)</td><td>UART P4 → C6</td></tr>
    <tr><td>15 (P4 GPIO5, RX)</td><td>22 (C6 U0TXD)</td><td>UART C6 → P4</td></tr>
    <tr><td>17 (P4 GPIO20)</td><td>24 (C6 IO9 / BOOT)</td><td>P4 nahrává firmware do C6 přes jeho ROM bootloader</td></tr></tbody></table>
    <p class="note">Nejdřív multimetrem ověřte pin 1 (3,3 V proti GND; pin 2 má 5 V). Pin 26 zůstává volný, reset C6 ovládá
    P4 uvnitř modulu. Role ESPHome funguje i bez propojek.</p>
    <p>USB-C vedle konektoru Ethernet: CH340, konzole a nahrávání firmwaru (otevření portu desku restartuje). Ethernet
    IP101 na 100 Mb/s; napájení přes USB-C.</p>""")

INSTALL = T(
    """<ol>
    <li>Back up the factory flash of the P4 (it also holds the factory C6 app):<br><code>scripts/backup_flash.sh /dev/cu.wchusbserial* esp32p4 backups/p4_backup.bin</code></li>
    <li>Wire the three JP1 jumpers (above).</li>
    <li>Flash the gateway: <code>scripts/flash_p4.sh /dev/cu.wchusbserial*</code> (the firmware comes from <code>scripts/build_all.sh</code> into <code>dist/</code>).</li>
    <li>Open <code>http://p4gw-xxxxxx.local/</code> (the name ends with the last digits of the MAC). The gateway starts in maintenance mode.</li>
    <li><b>System › Maintenance › Back up C6 flash</b> (about 3.5 min) and keep the file: it is the only copy of the factory C6 firmware.</li>
    <li><b>System › User management › Users › New user</b>: the first account is the admin. From then on the gateway asks for sign-in.</li>
    <li><b>Role</b>: choose the role. Zigbee roles flash the C6 (about 25 s), Thread roles take about 15 s.</li></ol>
    <p class="note"><b>Flash layout v3</b> (from v1.6): the P4 keeps only the small C6 bundles; the large C6 firmwares come from the release package, which the gateway downloads when the C6 needs them (or an upload of <code>c6fw.bin</code>). Boards with the older layout move once over USB: <code>scripts/migrate_layout_v3.sh &lt;port&gt;</code> (settings, Thread network, users and recordings stay).</p>""",
    """<ol>
    <li>Zazálohujte tovární flash P4 (obsahuje i tovární aplikaci C6):<br><code>scripts/backup_flash.sh /dev/cu.wchusbserial* esp32p4 backups/p4_backup.bin</code></li>
    <li>Zapojte tři propojky JP1 (viz výše).</li>
    <li>Nahrajte bránu: <code>scripts/flash_p4.sh /dev/cu.wchusbserial*</code> (firmware vytvoří <code>scripts/build_all.sh</code> do <code>dist/</code>).</li>
    <li>Otevřete <code>http://p4gw-xxxxxx.local/</code> (název končí posledními číslicemi MAC). Brána naběhne v režimu údržby.</li>
    <li><b>Systém › Údržba › Zálohovat flash C6</b> (asi 3,5 min) a soubor si uschovejte: je to jediná kopie továrního firmwaru C6.</li>
    <li><b>Systém › Správa uživatelů › Uživatelé › Nový uživatel</b>: první účet je správce. Od té chvíle brána vyžaduje přihlášení.</li>
    <li><b>Role</b>: zvolte roli. Role Zigbee nahrají firmware do C6 (asi 25 s), role Thread trvají asi 15 s.</li></ol>
    <p class="note"><b>Rozložení flash v3</b> (od v1.6): P4 drží jen malé balíky C6; velké firmwary C6 pocházejí z balíku vydání, který brána stáhne, když je C6 potřebuje (nebo nahrání <code>c6fw.bin</code>). Desky se starším rozložením se jednorázově převedou přes USB: <code>scripts/migrate_layout_v3.sh &lt;port&gt;</code> (nastavení, síť Thread, uživatelé i nahrávky zůstanou).</p>""")

UI_BASICS = T(
    """<p><b>Header</b>: status tags, outlined when the link works and grey when it is off. Point at a tag (or tap it) for
    details. From the left: LAN, the active role, its client (Zigbee2MQTT or Home Assistant over the OTBR REST API), the
    P4 Gateway integration (HA), MQTT and internet; on the right the uptime of the gateway with the Zigbee2MQTT session or
    the C6 uptime, and the gateway clock.</p>
    <p><b>Menu</b> on the left: sections and their pages; pages of a role that is not active are greyed out. At the bottom:
    language (flags), theme (system, light, dark), the signed-in user (My account, sign-out) and <b>Restart</b>, which
    after a confirmation resets the C6 radio and right after it the P4.</p>
    <p>A page the account may only read shows a note and its buttons that change something are disabled. Hidden pages are
    not in the menu at all.</p>""",
    """<p><b>Hlavička</b>: stavové štítky, orámované, když spojení funguje, šedé, když je vypnuté. Najetím (nebo klepnutím) se
    zobrazí podrobnosti. Zleva: LAN, aktivní role, její klient (Zigbee2MQTT nebo Home Assistant přes OTBR REST API),
    integrace P4 Gateway (HA), MQTT a internet; vpravo doba běhu brány s relací Zigbee2MQTT nebo dobou běhu C6 a hodiny
    brány.</p>
    <p><b>Menu</b> vlevo: sekce a jejich stránky; stránky neaktivní role jsou šedé. Dole: jazyk (vlajky), vzhled (podle
    systému, světlý, tmavý), přihlášený uživatel (Můj účet, odhlášení) a <b>Restartovat</b>, které po potvrzení
    restartuje rádio C6 a hned po něm P4.</p>
    <p>Stránka, kterou účet smí jen číst, má upozornění a tlačítka, která něco mění, jsou vypnutá. Skryté stránky v menu
    vůbec nejsou.</p>""")

# Pages of the web UI: route -> (title, intro, {card title: description}, howto)
PAGES = [
    ("overview", "view-dashboard-outline", T("Overview", "Přehled"),
     T("The state of the gateway at a glance.", "Stav brány na první pohled."),
     {"ROLE": T("Active role, its state and the client using it.", "Aktivní role, její stav a klient, který ji používá."),
      "NETWORK": T("Ethernet link, IPv4 and IPv6 addresses, hostname.", "Linka Ethernet, adresy IPv4 a IPv6, název zařízení."),
      "ESP32-P4": T("Firmware version, uptime, temperature, memory and CPU load.", "Verze firmwaru, doba běhu, teplota, paměť a zatížení CPU."),
      "ESP32-C6 RADIO": T("Firmware of the radio, its temperature and uptime (telemetry of the role firmware).", "Firmware rádia, jeho teplota a doba běhu (telemetrie firmwaru role).")},
     None, ["overview", "overview__zigbee", "overview__router"]),
    ("role", "swap-horizontal", T("Role", "Role"),
     T("Choose what the gateway does. Switching stores the role and restarts the gateway; Zigbee network data and the Thread network are kept.",
       "Volba, co brána dělá. Přepnutí uloží roli a restartuje bránu; data sítě Zigbee i síť Thread zůstávají zachované."),
     {"SELECT ROLE": T("One card per role with what runs on the P4, the C6 and what to set up in Home Assistant. <b>Switch</b> asks for a confirmation.",
                       "Karta pro každou roli s tím, co běží na P4, na C6 a co nastavit v Home Assistant. <b>Přepnout</b> si vyžádá potvrzení.")},
     T("""<ol><li>Pick the role and press <b>Switch</b>.</li><li>The gateway restarts; a dialog shows the progress (C6 flashing for Zigbee).</li>
       <li>Set up Home Assistant as shown under Home Assistant › Guide.</li></ol>
       <p>Also possible: the <i>Role</i> select of the P4 Gateway integration, Telegram <code>/role zigbee_coordinator</code>, MQTT <code>p4gw/&lt;host&gt;/role/set</code>.</p>""",
       """<ol><li>Zvolte roli a stiskněte <b>Přepnout</b>.</li><li>Brána se restartuje; dialog ukazuje průběh (u Zigbee nahrávání C6).</li>
       <li>Home Assistant nastavte podle Home Assistant › Návod.</li></ol>
       <p>Jinak také: výběr <i>Role</i> v integraci P4 Gateway, Telegram <code>/role zigbee_coordinator</code>, MQTT <code>p4gw/&lt;host&gt;/role/set</code>.</p>"""),
     ["role", "role__zigbee"]),
    ("thread/info", "information-outline", T("Thread › Information", "Thread › Informace"),
     T("State of the Thread network (Thread roles only).", "Stav sítě Thread (jen v rolích Thread)."),
     {"THREAD": T("Role in the network (leader, router, child), network name, channel, PAN ID, addresses.", "Role v síti (leader, router, child), název sítě, kanál, PAN ID, adresy."),
      "BORDER ROUTING": T("Prefixes routed to the LAN (OMR), NAT64, SRP server and the last contact of Home Assistant.", "Prefixy směrované do LAN (OMR), NAT64, SRP server a poslední kontakt Home Assistant.")},
     None, ["thread_info"]),
    ("thread/topology", "graph-outline", T("Thread › Topology", "Thread › Topologie"),
     T("Map of the mesh: routers and their children, link quality; device names from the Matter Server when available.",
       "Mapa sítě: routery a jejich potomci, kvalita linek; názvy zařízení z Matter Serveru, je-li dostupný."),
     {"NETWORK MAP": T("Ring or signal layout (shorter edge = better link), zoom and pan; point at a node for details.", "Rozložení do kruhu nebo podle signálu (kratší hrana = lepší linka), zoom a posun; najetím na uzel podrobnosti."),
      "DEVICES IN THE NETWORK": T("Matter devices with vendor, product and firmware.", "Zařízení Matter s výrobcem, produktem a firmwarem."),
      "NEIGHBORS": T("Direct neighbours of the gateway with RSSI and link quality.", "Přímí sousedé brány s RSSI a kvalitou linky."),
      "ROUTERS": T("All routers of the partition.", "Všechny routery oddílu sítě.")},
     None, ["thread_topology"]),
    ("thread/diag", "stethoscope", T("Thread › Diagnostics", "Thread › Diagnostika"),
     T("The radio co-processor and the OpenThread command line.", "Rádiový koprocesor a příkazová řádka OpenThread."),
     {"RADIO CO-PROCESSOR (RCP)": T("RCP version and failures; after a failure the gateway reflashes the C6 by itself.", "Verze RCP a výpadky; po výpadku brána sama znovu nahraje C6."),
      "MAC COUNTERS": T("Frames sent and received, retries and errors.", "Odeslané a přijaté rámce, opakování a chyby."),
      "OPENTHREAD CLI": T("Commands of the OpenThread CLI, e.g. <code>state</code>, <code>neighbor table</code>, <code>ping</code>.", "Příkazy OpenThread CLI, např. <code>state</code>, <code>neighbor table</code>, <code>ping</code>.")},
     None, ["thread_diag"]),
    ("thread/settings", "cog-outline", T("Thread › Settings", "Thread › Nastavení"),
     T("Dataset of the network and transmit power.", "Dataset sítě a vysílací výkon."),
     {"NETWORK DATASET": T("Name, channel, PAN IDs; the network key and TLVs only on request. <b>Apply dataset</b> joins another network.", "Název, kanál, PAN ID; klíč sítě a TLV jen na vyžádání. <b>Použít dataset</b> připojí bránu do jiné sítě."),
      "TRANSMIT POWER": T("-15 to 20 dBm, kept across restarts.", "-15 až 20 dBm, platí i po restartu.")},
     T("""<p><b>Join the preferred network of Home Assistant</b> (one Thread network instead of two):</p><ol>
       <li>Home Assistant: Settings › Devices & services › Thread › (network) › show the active dataset TLVs and copy them.</li>
       <li>Gateway: Thread › Settings › paste into <b>Apply dataset</b> and confirm.</li></ol>""",
       """<p><b>Připojení do preferované sítě Home Assistant</b> (jedna síť Thread místo dvou):</p><ol>
       <li>Home Assistant: Nastavení › Zařízení a služby › Thread › (síť) › zobrazit TLV aktivního datasetu a zkopírovat.</li>
       <li>Brána: Thread › Nastavení › vložit do <b>Použít dataset</b> a potvrdit.</li></ol>"""),
     ["thread_settings"]),
    ("zigbee/info", "information-outline", T("Zigbee › Information", "Zigbee › Informace"),
     T("The coordinator bridge, or the router and its network (Zigbee roles only).", "Most koordinátoru nebo router a jeho síť (jen v rolích Zigbee)."),
     {"COORDINATOR BRIDGE": T("TCP port 6638 for Zigbee2MQTT, the connected client and the session.", "TCP port 6638 pro Zigbee2MQTT, připojený klient a relace."),
      "ZIGBEE ROUTER": T("Router state, parent, channel and PAN.", "Stav routeru, rodič, kanál a PAN."),
      "NETWORK MEMBERSHIP": T("<b>Join network (steer)</b> after permit join in the coordinator; leave or factory reset.", "<b>Připojit do sítě (steer)</b> po permit join v koordinátoru; opuštění sítě nebo tovární reset."),
      "TRANSMIT POWER": T("Transmit power of the router.", "Vysílací výkon routeru.")},
     None, ["zigbee_info__zigbee", "zigbee_info__router"]),
    ("zigbee/diag", "stethoscope", T("Zigbee › Diagnostics", "Zigbee › Diagnostika"),
     T("Traffic of the bridge and the C6 link.", "Provoz mostu a linka k C6."),
     {"TRAFFIC": T("Bytes and frames between Zigbee2MQTT and the NCP, dropped data.", "Bajty a rámce mezi Zigbee2MQTT a NCP, zahozená data."),
      "C6 LINK": T("UART speed, errors and C6 resets; <b>Reset C6</b>.", "Rychlost UART, chyby a resety C6; <b>Reset C6</b>."),
      "ZIGBEE ROUTER": T("Router counters and log.", "Čítače a log routeru.")},
     None, ["zigbee_diag__zigbee", "zigbee_diag__router"]),
    ("zigbee/backups", "backup-restore", T("Zigbee › Network backups", "Zigbee › Zálohy sítí"),
     T("Copies of the coordinator and router network data kept in the gateway flash; made before leaving a Zigbee role and automatically (daily or weekly).",
       "Kopie dat sítě koordinátoru a routeru uložené ve flash brány; vytváří se před opuštěním role Zigbee a automaticky (denně nebo týdně)."),
     {"ZIGBEE NETWORK BACKUPS": T("Generation, size and time of each copy; download; <b>Back up now</b>.", "Generace, velikost a čas každé kopie; stažení; <b>Zálohovat teď</b>.")},
     None, ["zigbee_backups__zigbee"]),
    ("zigbee/network", "graph-outline", T("Zigbee › Network and devices", "Zigbee › Síť a zařízení"),
     T("The Zigbee network seen through Zigbee2MQTT over MQTT (works in any role).", "Síť Zigbee pohledem Zigbee2MQTT přes MQTT (funguje v každé roli)."),
     {"ZIGBEE2MQTT": T("Base topic and bridge state; needs the MQTT connection of the gateway.", "Základní téma a stav mostu; potřebuje MQTT spojení brány."),
      "PERMIT JOIN": T("Open the network for new devices for a while.", "Otevře síť na chvíli novým zařízením."),
      "NETWORK MAP": T("Network map from Zigbee2MQTT with link quality.", "Mapa sítě ze Zigbee2MQTT s kvalitou linek."),
      "ZIGBEE DEVICES": T("Devices with model, power source and last seen.", "Zařízení s modelem, napájením a posledním kontaktem.")},
     None, ["zigbee_network", "zigbee_network__zigbee"]),
    ("sound/live", "ear-hearing", T("Sound › Live", "Zvuk › Živě"),
     T("Microphone, noise level and the recognition model.", "Mikrofon, hlučnost a model rozpoznávání."),
     {"MICROPHONE": T("On or off, gain, level gate (the model runs only above it), live level, a 5 s recording.", "Zapnutí, zesílení, prahová úroveň (model běží jen nad ní), živá hladina, nahrávka 5 s."),
      "NOISE LEVEL": T("A-weighted level LAS, LAeq and LAmax over 60 s in dB(A). <b>Calibrate</b>: enter the reading of a sound level meter.", "Hladina vážená A: LAS, LAeq a LAmax za 60 s v dB(A). <b>Kalibrovat</b>: zadejte údaj hlukoměru."),
      "SOUND RECOGNITION (YAMNET)": T("Recognition on or off, run time and CPU share of the model, model upload.", "Zapnutí rozpoznávání, doba výpočtu a podíl CPU, nahrání modelu."),
      "HEARD NOW": T("Top classes of the last windows with their scores.", "Nejvyšší třídy posledních oken s jejich skóre.")},
     T("""<p><b>Calibrate the noise level</b>: put a sound level meter (or a phone app) next to the gateway, set it to A-weighting and slow,
       wait for a steady sound and enter its reading under Noise level › Calibrate. Without calibration the level is an estimate (±6 dB).</p>""",
       """<p><b>Kalibrace hlučnosti</b>: hlukoměr (nebo aplikaci v telefonu) dejte vedle brány, nastavte vážení A a pomalou odezvu,
       počkejte na ustálený zvuk a jeho údaj zadejte v Hlučnost › Kalibrovat. Bez kalibrace je hladina odhad (±6 dB).</p>"""),
     ["sound_live"]),
    ("sound/events", "bell-ring-outline", T("Sound › Events", "Zvuk › Události"),
     T("Groups of sound classes sent to Home Assistant as events (dog barking, doorbell, glass breaking, smoke alarm…).",
       "Skupiny tříd zvuků, které jdou do Home Assistant jako události (štěkání psa, zvonek, rozbití skla, kouřový hlásič…)."),
     {"EVENTS SENT TO HOME ASSISTANT": T("Presets (household, security, animals, minimal), each event with classes, threshold, windows, hold and cooldown; edit, add, export and import.",
                                         "Předvolby (domácnost, zabezpečení, zvířata, minimální), každá událost s třídami, prahem, okny, držením a útlumem; úprava, přidání, export a import.")},
     T("""<ol><li>Pick a preset or press <b>Add event</b>.</li><li>Choose the classes (Sound selection shows all 521), threshold (0.5 to start) and timing.</li>
       <li>Save: Home Assistant gets an event entity and a binary sensor per enabled event.</li></ol>""",
       """<ol><li>Zvolte předvolbu nebo stiskněte <b>Přidat událost</b>.</li><li>Vyberte třídy (Výběr zvuků ukazuje všech 521), práh (pro začátek 0,5) a časování.</li>
       <li>Uložte: Home Assistant dostane pro každou zapnutou událost entitu události a binární senzor.</li></ol>"""),
     ["sound_events"]),
    ("sound/log", "history", T("Sound › Detection log", "Zvuk › Log detekcí"),
     T("Last detections with class, score, level and duration.", "Poslední detekce s třídou, skóre, hladinou a délkou."),
     {"DETECTION LOG": T("Filter by event; a row shows which class triggered it.", "Filtr podle události; řádek ukazuje, která třída ji spustila.")}, None, ["sound_log"]),
    ("sound/classes", "playlist-check", T("Sound › Sound selection", "Zvuk › Výběr zvuků"),
     T("All 521 AudioSet classes as a tree; excluded classes never trigger anything.", "Všech 521 tříd AudioSet jako strom; vyřazené třídy nikdy nic nespustí."),
     {"ALL SOUNDS": T("Search, Czech names, the events that use a class.", "Hledání, české názvy, události, které třídu používají.")}, None, ["sound_classes"]),
    ("sound/learn", "school-outline", T("Sound › Learned sounds", "Zvuk › Naučené zvuky"),
     T("Own sounds the model does not know: recorded samples and a profile (embedding similarity or a tone).",
       "Vlastní zvuky, které model nezná: nahrané vzorky a profil (podobnost embeddingu nebo tón)."),
     {"RECORD": T("Record 0.96 s samples of the sound, or upload a WAV.", "Nahrání vzorků 0,96 s zvuku nebo nahrání WAV."),
      "BACKGROUND": T("Record the normal background; the suggested threshold lies between it and the samples.", "Nahrání běžného pozadí; navržený práh leží mezi ním a vzorky."),
      "LEARNED SOUNDS": T("Profiles with samples, threshold and test.", "Profily se vzorky, prahem a testem.")}, None, ["sound_learn"]),
    ("ha/guide", "book-open-variant", T("Home Assistant › Guide", "Home Assistant › Návod"),
     T("Steps for Home Assistant with the current address of the gateway; one-click buttons for the integration.",
       "Postup pro Home Assistant se současnou adresou brány; tlačítka na jedno kliknutí pro integraci."),
     {"P4 GATEWAY INTEGRATION": T("Open in HACS, start the setup.", "Otevřít v HACS, spustit nastavení."),
      "THREAD BORDER ROUTER → HOME ASSISTANT": T("OTBR integration URL, Thread and Matter steps.", "URL integrace OTBR, kroky pro Thread a Matter."),
      "ZIGBEE COORDINATOR → ZIGBEE2MQTT (RECOMMENDED)": T("Zigbee2MQTT configuration.", "Konfigurace Zigbee2MQTT.")},
     None, ["ha_guide", "ha_guide__zigbee"]),
    ("ha/mqtt", "transit-connection-variant", T("Home Assistant › MQTT", "Home Assistant › MQTT"),
     T("Optional MQTT link with Home Assistant discovery (the integration does not need it).", "Volitelné spojení MQTT s discovery pro Home Assistant (integrace ho nepotřebuje)."),
     {"MQTT (OPTIONAL)": T("Broker URI (mqtt://, mqtts://, ws://, wss://), user, password, TLS certificate, advanced options; state and last error.",
                           "URI brokeru (mqtt://, mqtts://, ws://, wss://), uživatel, heslo, certifikát TLS, pokročilé volby; stav a poslední chyba.")},
     T("""<p>With the P4 Gateway integration in use, turn off <b>Home Assistant discovery</b> here, otherwise every entity exists twice.
       MQTT is still needed for Zigbee › Network and devices (Zigbee2MQTT topics).</p>""",
       """<p>Když používáte integraci P4 Gateway, vypněte zde <b>Home Assistant discovery</b>, jinak bude každá entita dvakrát.
       MQTT je dál potřeba pro Zigbee › Síť a zařízení (témata Zigbee2MQTT).</p>"""),
     ["ha_mqtt"]),
    ("ha/esphome", "bluetooth", T("Home Assistant › ESPHome", "Home Assistant › ESPHome"),
     T("The ESPHome Bluetooth proxy role: the gateway restarts into the ESPHome image.", "Role ESPHome Bluetooth proxy: brána se restartuje do obrazu ESPHome."),
     {"ESPHOME BLE PROXY ROLE": T("Image version and the way back (<i>Gateway role</i> select and <i>Return to gateway</i> button of the ESPHome device in Home Assistant).",
                                  "Verze obrazu a cesta zpět (výběr <i>Gateway role</i> a tlačítko <i>Return to gateway</i> zařízení ESPHome v Home Assistant).")},
     None, ["ha_esphome"]),
    ("system/info", "information-outline", T("System › Information", "Systém › Informace"),
     T("Hardware and firmware details.", "Podrobnosti o hardwaru a firmwaru."),
     {"FIRMWARE SLOTS": T("Gateway A/B, ESPHome: version, state, which runs and which boots.", "Brána A/B, ESPHome: verze, stav, který běží a který startuje."),
      "ESP32-P4": T("Chip revision, CPU, temperature, flash and PSRAM.", "Revize čipu, CPU, teplota, flash a PSRAM."),
      "MEMORY": T("Internal RAM and PSRAM, minimum since start, NVS.", "Interní RAM a PSRAM, minimum od startu, NVS."),
      "FIRMWARE": T("Version, build, ESP-IDF, reset reason.", "Verze, sestavení, ESP-IDF, důvod resetu."),
      "ESP32-C6 RADIO": T("MAC, revision, flash, bundles stored for the C6.", "MAC, revize, flash, balíky firmwaru uložené pro C6.")}, None, ["system_info"]),
    ("system/lan", "ethernet", T("System › LAN", "Systém › LAN"),
     T("Ethernet settings and traffic.", "Nastavení a provoz Ethernetu."),
     {"LAN SETTINGS": T("DHCP or a static address with netmask, gateway and DNS; link speed and duplex.", "DHCP nebo statická adresa s maskou, bránou a DNS; rychlost a duplex linky."),
      "CURRENT STATE": T("What is in effect now.", "Co právě platí."),
      "TRAFFIC": T("RX/TX rate, totals since start and the split per service (web UI, OTBR REST, Zigbee2MQTT, MQTT, mDNS, DNS/NTP, HTTPS, Thread routed).",
                   "Rychlost RX/TX, součty od startu a rozpad podle služeb (web UI, OTBR REST, Zigbee2MQTT, MQTT, mDNS, DNS/NTP, HTTPS, směrovaný Thread).")},
     T("""<ol><li>Choose <b>Static</b>, fill in the address, netmask, gateway and DNS, press <b>Apply</b>.</li>
       <li>The page opens on the new address; confirm the banner within 120 s. Without confirmation the previous settings come back by themselves.</li>
       <li>Zigbee2MQTT uses the address: change it there too. The P4 Gateway integration finds the new address over mDNS.</li></ol>""",
       """<ol><li>Zvolte <b>Statická</b>, vyplňte adresu, masku, bránu a DNS a stiskněte <b>Použít</b>.</li>
       <li>Stránka se otevře na nové adrese; do 120 s potvrďte lištu nahoře. Bez potvrzení se vrátí předchozí nastavení samo.</li>
       <li>Zigbee2MQTT používá adresu: změňte ji i tam. Integrace P4 Gateway novou adresu najde přes mDNS.</li></ol>"""),
     ["system_lan"]),
    ("system/firmware", "upload", T("System › Firmware", "Systém › Firmware"),
     T("Updates of all parts.", "Aktualizace všech částí."),
     {"ONLINE UPDATE": T("Manifest URL (<b>GitHub Releases</b> fills in the address of the latest release), check, then the gateway firmware and the <b>C6 package</b> separately: versions installed and offered of every C6 firmware, build (commit, date, ESP-IDF) and release notes. <b>Releases for this gateway</b>: older releases that run on the flash layout of the board, the gateway or the C6 package installable separately (rollback).",
                         "URL manifestu (<b>GitHub Releases</b> doplní adresu posledního vydání), kontrola, pak zvlášť firmware brány a <b>balík C6</b>: nainstalované a nabízené verze všech firmwarů C6, sestavení (commit, datum, ESP-IDF) a poznámky k vydání. <b>Vydání pro tuto bránu</b>: starší vydání, která běží na layoutu flash desky, bránu nebo balík C6 jde nainstalovat zvlášť (návrat zpět)."),
      "MANUAL UPDATE": T("Upload without internet: <code>p4gw.bin</code> (written to the other slot, confirmed after 60 s, otherwise rolled back), <code>c6fw.bin</code> (the C6 gets the new firmware on the next start) and <code>esphome.bin</code> for the ESPHome role.",
                         "Nahrání bez internetu: <code>p4gw.bin</code> (zapíše se do druhého slotu, potvrdí se po 60 s, jinak se vrátí předchozí), <code>c6fw.bin</code> (C6 dostane nový firmware při dalším startu) a <code>esphome.bin</code> pro roli ESPHome.")},
     T("""<ol><li>Set the manifest URL (your web server, or <b>GitHub Releases</b> once the repository is public) and press <b>Check</b>.</li>
       <li><b>Install gateway firmware</b> first when both are newer: a C6 package of a newer format is offered only to a gateway that reads it.</li>
       <li><b>Install C6 package</b>: the gateway downloads it into RAM, checks its sha256 against the manifest, stores it and restarts; the C6 then gets the new firmware (about 2 minutes without Thread or Zigbee).</li></ol>
       <p>Without internet the same works by uploading <code>c6fw.bin</code> in the Manual update card. Also from Home Assistant (<i>C6 radio firmware</i> update entity) and Telegram (<code>/update</code>, <code>/install c6</code>).</p>""",
       """<ol><li>Nastavte URL manifestu (vlastní webový server, nebo <b>GitHub Releases</b>, až bude repozitář veřejný) a stiskněte <b>Zkontrolovat</b>.</li>
       <li>Když jsou novější obě části, instalujte nejdřív <b>firmware brány</b>: balík C6 novějšího formátu se nabídne až bráně, která ho umí přečíst.</li>
       <li><b>Instalovat balík C6</b>: brána ho stáhne do RAM, ověří sha256 proti manifestu, uloží a restartuje se; C6 pak dostane nový firmware (asi 2 minuty bez Thread nebo Zigbee).</li></ol>
       <p>Bez internetu to samé jde nahráním <code>c6fw.bin</code> v kartě Ruční aktualizace. Také z Home Assistant (entita aktualizace <i>Firmware rádia C6</i>) a z Telegramu (<code>/update</code>, <code>/install c6</code>).</p>"""),
     ["system_firmware"]),
    ("system/diag", "stethoscope", T("System › Diagnostics", "Systém › Diagnostika"),
     T("Tools when something does not work.", "Nástroje, když něco nefunguje."),
     {"PING": T("Ping a host from the gateway.", "Ping na hostitele z brány."),
      "DIAGNOSTICS": T("Core dump after a crash (download, erase), log before the last restart.", "Core dump po pádu (stažení, smazání), log před posledním restartem."),
      "LOG": T("Live log with filters by source, level and tag; save to a file.", "Živý log s filtry podle zdroje, úrovně a tagu; uložení do souboru.")}, None, ["system_diag"]),
    ("system/netscan", "radar", T("System › Network surroundings", "Systém › Síťové okolí"),
     T("Scan of the 2.4 GHz band to place a Thread, Zigbee or Wi-Fi network.", "Sken pásma 2,4 GHz pro umístění sítě Thread, Zigbee nebo Wi-Fi."),
     {"SCAN": T("Quick (Thread roles, no outage, about 13 s) or full (any role, about a minute without the role).", "Rychlý (role Thread, bez výpadku, asi 13 s) nebo úplný (každá role, asi minutu bez role)."),
      "RECOMMENDATION": T("Best channels for Thread and Zigbee.", "Nejlepší kanály pro Thread a Zigbee."),
      "2.4 GHZ SPECTRUM": T("Wi-Fi and 802.15.4 channels together.", "Kanály Wi-Fi a 802.15.4 společně."),
      "IEEE 802.15.4 CHANNELS": T("Energy, networks and rating per channel 11–26.", "Energie, sítě a hodnocení každého kanálu 11–26."),
      "THREAD AND ZIGBEE NETWORKS": T("Networks found nearby.", "Sítě nalezené v okolí."),
      "WI-FI NETWORKS": T("Access points with channel and signal (full scan).", "Přístupové body s kanálem a signálem (úplný sken).")}, None, ["system_netscan"]),
    ("system/settings", "cog-outline", T("System › Settings", "Systém › Nastavení"),
     T("Hostname, Matter Server and the settings backup.", "Název zařízení, Matter Server a záloha nastavení."),
     {"HOME ASSISTANT AND MATTER": T("Home Assistant found over mDNS; Matter Server URL for device names in the Thread map.", "Home Assistant nalezený přes mDNS; URL Matter Serveru pro názvy zařízení v mapě Thread."),
      "SETTINGS": T("Hostname (applies after a restart). Accounts and passwords are in User management.", "Název zařízení (platí po restartu). Účty a hesla jsou ve Správě uživatelů."),
      "SETTINGS BACKUP": T("Export and import of all settings; passwords, accounts and tokens only when chosen.", "Export a import všech nastavení; hesla, účty a tokeny jen na vyžádání.")}, None, ["system_settings"]),
    ("system/maint", "wrench-outline", T("System › Maintenance", "Systém › Údržba"),
     T("Restart, C6 reflash and backup.", "Restart, přeflashování a záloha C6."),
     {"MAINTENANCE": T("<b>Restart</b> (P4), <b>Reflash C6 now</b> (after an RCP or NCP failure), <b>Back up C6 flash</b> (in maintenance mode, about 3.5 min).",
                       "<b>Restartovat</b> (P4), <b>Přeflashovat C6</b> (po výpadku RCP nebo NCP), <b>Zálohovat flash C6</b> (v režimu údržby, asi 3,5 min).")}, None, ["system_maint"]),
    ("system/notify", "bell-cog-outline", T("System › Notifications", "Systém › Notifikace"),
     T("The Telegram bot (admins) and where Home Assistant notifications are set.", "Telegram bot (správci) a kde se nastavují notifikace pro Home Assistant."),
     {"TELEGRAM BOT": T("Token from @BotFather, on/off, time zone for quiet hours; state and paired users.", "Token od @BotFather, zapnutí, časové pásmo pro tiché hodiny; stav a spárovaní uživatelé."),
      "HOME ASSISTANT": T("The integration gets every notification; levels per topic are set in its options.", "Integrace dostává každou notifikaci; úrovně pro témata se nastavují v jejích volbách.")},
     T("""<ol><li>Telegram: open <b>@BotFather</b>, send <code>/newbot</code>, choose a name and a user name ending with <code>bot</code>.</li>
       <li>Paste the token here, tick <b>Bot on</b>, save. The state turns to <i>running, @yourbot</i>.</li>
       <li>Every user pairs a chat in My account › Telegram and notifications.</li></ol>""",
       """<ol><li>Telegram: otevřete <b>@BotFather</b>, pošlete <code>/newbot</code>, zvolte jméno a uživatelské jméno končící <code>bot</code>.</li>
       <li>Token vložte sem, zaškrtněte <b>Bot zapnutý</b> a uložte. Stav se změní na <i>běží, @vašbot</i>.</li>
       <li>Každý uživatel si spáruje chat v Můj účet › Telegram a notifikace.</li></ol>"""),
     ["system_notify"]),
    ("system/users/list", "account-multiple-outline", T("System › User management › Users", "Systém › Správa uživatelů › Uživatelé"),
     T("Accounts of the web UI (admins only).", "Účty webového rozhraní (jen správci)."),
     {"USERS": T("Name, role, 2FA, state (active, locked, disabled, must change the password), last sign-in, sessions; <b>New user</b>, <b>Edit</b>, delete.",
                 "Jméno, role, 2FA, stav (aktivní, zamčený, zablokovaný, musí změnit heslo), poslední přihlášení, relace; <b>Nový uživatel</b>, <b>Upravit</b>, smazání.")},
     T("""<ol><li><b>New user</b>: name, role and a temporary password (generated); the user changes it at the first sign-in.</li>
       <li><b>Edit</b>: role, disable, new password, turn off 2FA (lost phone), unlock, and <b>own permissions</b> page by page instead of the role.</li></ol>
       <p>Admins always have every right; the last admin cannot be removed or demoted.</p>""",
       """<ol><li><b>Nový uživatel</b>: jméno, role a dočasné heslo (vygenerované); uživatel ho změní při prvním přihlášení.</li>
       <li><b>Upravit</b>: role, zablokování, nové heslo, vypnutí 2FA (ztracený telefon), odemčení a <b>vlastní oprávnění</b> po stránkách místo role.</li></ol>
       <p>Správce má vždy všechna práva; posledního správce nejde odebrat ani mu změnit roli.</p>"""),
     ["system_users_list"]),
    ("system/users/roles", "shield-account-outline", T("System › User management › Roles and permissions", "Systém › Správa uživatelů › Role a oprávnění"),
     T("What the user and viewer roles see and may change, page by page: hidden, read or edit. The gateway enforces it for every API call.",
       "Co role uživatel a divák vidí a smí měnit, po stránkách: skryto, číst nebo upravovat. Brána to vynucuje u každého volání API."),
     {"ROLES AND PERMISSIONS": T("Matrix of pages × roles; <b>Defaults</b> returns the default levels.", "Matice stránek × rolí; <b>Výchozí</b> vrátí výchozí úrovně.")}, None, ["system_users_roles"]),
    ("system/users/tokens", "key-outline", T("System › User management › API tokens", "Systém › Správa uživatelů › API tokeny"),
     T("Tokens for Home Assistant and scripts (<code>Authorization: Bearer p4gw_…</code>), each with a role; shown only once.",
       "Tokeny pro Home Assistant a skripty (<code>Authorization: Bearer p4gw_…</code>), každý s rolí; zobrazí se jen jednou."),
     {"API TOKENS": T("Name, role, created, last use; <b>New token</b>, <b>Revoke</b>.", "Název, role, vytvoření, poslední použití; <b>Nový token</b>, <b>Zneplatnit</b>.")}, None, ["system_users_tokens"]),
    ("system/users/requests", "lock-reset", T("System › User management › Reset requests", "Systém › Správa uživatelů › Žádosti o reset"),
     T("Requests sent from the sign-in page (Forgot password); admins also get a notification.", "Žádosti odeslané z přihlašovací stránky (Zapomenuté heslo); správci dostanou i notifikaci."),
     {"PASSWORD RESET REQUESTS": T("<b>Set password</b> (temporary, changed at the next sign-in) or <b>Dismiss</b>.", "<b>Nastavit heslo</b> (dočasné, změní se při dalším přihlášení) nebo <b>Zamítnout</b>.")}, None, ["system_users_requests"]),
    ("system/users/policy", "shield-lock-outline", T("System › User management › Security", "Systém › Správa uživatelů › Zabezpečení"),
     T("Rules of the sign-in.", "Pravidla přihlášení."),
     {"SIGN-IN SECURITY": T("Sign-out after inactivity, how long \"Stay signed in\" lasts, minimum password length, attempts before a lock and its duration, two-factor sign-in required per role.",
                            "Odhlášení po nečinnosti, jak dlouho platí „Zůstat přihlášen“, minimální délka hesla, pokusy do zamčení a jeho délka, povinné dvoufázové přihlášení podle role.")},
     None, ["system_users_policy"]),
    ("profile", "account-circle-outline", T("My account", "Můj účet"),
     T("Own account: password, two-factor sign-in, Telegram and notifications.", "Vlastní účet: heslo, dvoufázové přihlášení, Telegram a notifikace."),
     {"ACCOUNT": T("Change password (other sessions end), sign out other sessions.", "Změna hesla (ostatní relace skončí), odhlášení ostatních relací."),
      "TWO-FACTOR SIGN-IN": T("Set up with Google Authenticator (or another TOTP app) and 10 recovery codes.", "Nastavení s Google Authenticator (nebo jinou aplikací TOTP) a 10 záložními kódy."),
      "TELEGRAM AND NOTIFICATIONS": T("Pair a chat, level per topic, quiet hours, language of the bot, test message.", "Spárování chatu, úroveň pro každé téma, tiché hodiny, jazyk bota, testovací zpráva.")},
     T("""<p><b>Two-factor sign-in</b>: <b>Set up</b> › scan the QR code › enter the 6-digit code › keep the recovery codes (each signs in once instead of the app code).</p>
       <p><b>Telegram</b>: <b>Pair</b> › open the link in Telegram and press Start (or send <code>/start &lt;code&gt;</code>). Then set the levels per topic and save.</p>""",
       """<p><b>Dvoufázové přihlášení</b>: <b>Nastavit</b> › naskenujte QR kód › zadejte 6místný kód › uložte si záložní kódy (každý jednou přihlásí místo kódu z aplikace).</p>
       <p><b>Telegram</b>: <b>Spárovat</b> › otevřete odkaz v Telegramu a stiskněte Start (nebo pošlete <code>/start &lt;kód&gt;</code>). Pak nastavte úrovně pro témata a uložte.</p>"""),
     ["profile"]),
]

HA = T(
    """<h3>P4 Gateway integration</h3>
    <ol><li>Install: <b>Open in HACS</b> (Home Assistant › Guide) or copy <code>custom_components/p4gw</code> to <code>/config/custom_components/</code>; restart Home Assistant.</li>
    <li>The gateway appears under Settings › Devices & services › <b>Discovered</b> (mDNS <code>_p4gw._tcp</code>), or add it: Add integration › P4 Gateway › address.</li>
    <li>With user accounts on the gateway it asks for an <b>API token</b>: create one under System › User management › API tokens (role admin, or user when the integration should not restart or update) and paste it. The token does not depend on any user or their 2FA.</li>
    <li><b>Configure</b> (integration options): per topic from which level a persistent notification appears (off, critical only, warnings and critical, all).</li></ol>
    <p>Entities: active role, Thread state, noise and noise max in dB(A), role select, sound recognition switch, one event and binary sensor per sound event,
    <b>Notification</b> event (for automations), firmware update and <b>C6 radio firmware</b> update, restart, identify, Zigbee backup and update check buttons, diagnostic sensors.</p>
    <h3>Thread and Matter</h3>
    <ol><li>Settings › Devices & services › Add integration › <b>OpenThread Border Router</b>, URL <code>http://&lt;gateway&gt;:8081</code>.</li>
    <li>Thread › Configure: mark the network as preferred, or join the gateway to the existing network (Thread › Settings › Apply dataset).</li>
    <li>Phone: Companion app › Sync Thread credentials. Matter: Matter Server add-on, Add device › Matter.</li>
    <li>Network: IPv6 and router advertisements on the Home Assistant host, no multicast filtering between Home Assistant and the gateway.</li></ol>
    <h3>Zigbee2MQTT</h3>
    <pre>serial:
  port: tcp://&lt;gateway&gt;:6638
  adapter: zboss
advanced:
  transmit_power: 20</pre>
    <p>One TCP client at a time. ZHA cannot use this coordinator (no ZBOSS support in Home Assistant).</p>
    <h3>ESPHome BLE proxy</h3>
    <p>Role › ESPHome BLE proxy; Home Assistant discovers the ESPHome node, enter its API key. Back with the <i>Gateway role</i> select and <i>Return to gateway</i>.</p>""",
    """<h3>Integrace P4 Gateway</h3>
    <ol><li>Instalace: <b>Otevřít v HACS</b> (Home Assistant › Návod) nebo zkopírujte <code>custom_components/p4gw</code> do <code>/config/custom_components/</code>; restartujte Home Assistant.</li>
    <li>Brána se objeví v Nastavení › Zařízení a služby › <b>Objeveno</b> (mDNS <code>_p4gw._tcp</code>), nebo ji přidejte: Přidat integraci › P4 Gateway › adresa.</li>
    <li>Když má brána uživatelské účty, integrace chce <b>API token</b>: vytvořte ho v Systém › Správa uživatelů › API tokeny (role správce, nebo uživatel, když integrace nemá restartovat ani aktualizovat) a vložte ho. Token nezávisí na žádném uživateli ani jeho 2FA.</li>
    <li><b>Konfigurovat</b> (volby integrace): pro každé téma, od jaké úrovně se zobrazí trvalé oznámení (vypnuto, jen kritické, varování a kritické, vše).</li></ol>
    <p>Entity: aktivní role, stav Thread, hluk a hluk max v dB(A), výběr role, přepínač rozpoznávání zvuků, událost a binární senzor pro každou zvukovou událost,
    událost <b>Notifikace</b> (pro automatizace), aktualizace firmwaru a <b>firmwaru rádia C6</b>, tlačítka restart, identifikace, záloha Zigbee a kontrola aktualizace, diagnostické senzory.</p>
    <h3>Thread a Matter</h3>
    <ol><li>Nastavení › Zařízení a služby › Přidat integraci › <b>OpenThread Border Router</b>, URL <code>http://&lt;brána&gt;:8081</code>.</li>
    <li>Thread › Konfigurovat: označte síť jako preferovanou, nebo bránu připojte do existující sítě (Thread › Nastavení › Použít dataset).</li>
    <li>Telefon: aplikace Companion › Synchronizovat přihlašovací údaje Thread. Matter: doplněk Matter Server, Přidat zařízení › Matter.</li>
    <li>Síť: IPv6 a router advertisements na hostiteli Home Assistant, žádné filtrování multicastu mezi Home Assistant a bránou.</li></ol>
    <h3>Zigbee2MQTT</h3>
    <pre>serial:
  port: tcp://&lt;brána&gt;:6638
  adapter: zboss
advanced:
  transmit_power: 20</pre>
    <p>Najednou jeden TCP klient. ZHA tento koordinátor použít neumí (Home Assistant nepodporuje ZBOSS).</p>
    <h3>ESPHome BLE proxy</h3>
    <p>Role › ESPHome BLE proxy; Home Assistant najde uzel ESPHome, zadejte jeho API klíč. Zpět výběrem <i>Gateway role</i> a tlačítkem <i>Return to gateway</i>.</p>""")

TELEGRAM = T(
    """<table><thead><tr><th>Command</th><th>What it does</th><th>Right</th><th>Confirmation</th></tr></thead><tbody>
    <tr><td><code>/help</code></td><td>commands the account may use</td><td>–</td><td>–</td></tr>
    <tr><td><code>/status</code></td><td>role, LAN, internet, MQTT, uptime, temperature</td><td>Overview, read</td><td>–</td></tr>
    <tr><td><code>/lan</code></td><td>link, addresses, traffic</td><td>System › LAN, read</td><td>–</td></tr>
    <tr><td><code>/thread</code>, <code>/zigbee</code></td><td>state of the role</td><td>Information, read</td><td>–</td></tr>
    <tr><td><code>/noise</code></td><td>noise in dB(A)</td><td>Sound › Live, read</td><td>–</td></tr>
    <tr><td><code>/events</code></td><td>last detections</td><td>Sound › Detection log, read</td><td>–</td></tr>
    <tr><td><code>/update</code></td><td>gateway and C6 versions, build, release notes</td><td>System › Firmware, read</td><td>–</td></tr>
    <tr><td><code>/install</code>, <code>/install c6</code></td><td>install the gateway firmware or the C6 package</td><td>System › Firmware, edit</td><td>yes</td></tr>
    <tr><td><code>/restart</code></td><td>restart C6 and P4</td><td>System › Maintenance, edit</td><td>yes</td></tr>
    <tr><td><code>/identify</code></td><td>blink the identify icon</td><td>Overview, edit</td><td>yes</td></tr>
    <tr><td><code>/role &lt;name&gt;</code></td><td>switch the role</td><td>Role, edit</td><td>yes</td></tr>
    <tr><td><code>/sound on|off</code></td><td>sound recognition</td><td>Sound › Live, edit</td><td>yes</td></tr>
    <tr><td><code>/mute 1h</code></td><td>mute sound notifications (<code>30m</code>, <code>1d</code>, <code>off</code>)</td><td>–</td><td>–</td></tr></tbody></table>
    <p>Confirmation: <b>Confirm</b> / <b>Cancel</b> buttons (2 minutes); for an account with two-factor sign-in the bot then asks for the authenticator code — send it as a normal message.
    Notifications arrive within about 5 s; security ones only to admins; in quiet hours only critical ones.</p>""",
    """<table><thead><tr><th>Příkaz</th><th>Co dělá</th><th>Oprávnění</th><th>Potvrzení</th></tr></thead><tbody>
    <tr><td><code>/help</code></td><td>příkazy, které účet smí použít</td><td>–</td><td>–</td></tr>
    <tr><td><code>/status</code></td><td>role, LAN, internet, MQTT, doba běhu, teplota</td><td>Přehled, číst</td><td>–</td></tr>
    <tr><td><code>/lan</code></td><td>linka, adresy, provoz</td><td>Systém › LAN, číst</td><td>–</td></tr>
    <tr><td><code>/thread</code>, <code>/zigbee</code></td><td>stav role</td><td>Informace, číst</td><td>–</td></tr>
    <tr><td><code>/noise</code></td><td>hluk v dB(A)</td><td>Zvuk › Živě, číst</td><td>–</td></tr>
    <tr><td><code>/events</code></td><td>poslední detekce</td><td>Zvuk › Log detekcí, číst</td><td>–</td></tr>
    <tr><td><code>/update</code></td><td>verze brány a C6, sestavení, poznámky k vydání</td><td>Systém › Firmware, číst</td><td>–</td></tr>
    <tr><td><code>/install</code>, <code>/install c6</code></td><td>instalace firmwaru brány nebo balíku C6</td><td>Systém › Firmware, upravovat</td><td>ano</td></tr>
    <tr><td><code>/restart</code></td><td>restart C6 a P4</td><td>Systém › Údržba, upravovat</td><td>ano</td></tr>
    <tr><td><code>/identify</code></td><td>rozblikání ikony identifikace</td><td>Přehled, upravovat</td><td>ano</td></tr>
    <tr><td><code>/role &lt;název&gt;</code></td><td>přepnutí role</td><td>Role, upravovat</td><td>ano</td></tr>
    <tr><td><code>/sound on|off</code></td><td>rozpoznávání zvuků</td><td>Zvuk › Živě, upravovat</td><td>ano</td></tr>
    <tr><td><code>/mute 1h</code></td><td>ztlumení notifikací zvuků (<code>30m</code>, <code>1d</code>, <code>off</code>)</td><td>–</td><td>–</td></tr></tbody></table>
    <p>Potvrzení: tlačítka <b>Potvrdit</b> / <b>Zrušit</b> (2 minuty); u účtu s dvoufázovým přihlášením si bot pak vyžádá kód z Authenticatoru — pošlete ho jako běžnou zprávu.
    Notifikace dorazí do zhruba 5 s; o zabezpečení jen správcům; v tichých hodinách jen kritické.</p>""")

RECOVERY = T(
    """<table><thead><tr><th>Situation</th><th>What to do</th></tr></thead><tbody>
    <tr><td>Role does not start</td><td>The UI shows maintenance with the reason; choose another role or fix it and restart.</td></tr>
    <tr><td>C6 does not answer</td><td>Check the JP1 jumpers, System › Maintenance › Reflash C6 now.</td></tr>
    <tr><td>Bad gateway update</td><td>Automatic rollback to the previous slot; otherwise <code>scripts/flash_p4.sh</code>.</td></tr>
    <tr><td>Crashes</td><td>System › Diagnostics: core dump and the log before the last restart.</td></tr>
    <tr><td>Static address that does not work</td><td>Not confirmed: back after 120 s by itself. Confirmed: <code>http://&lt;host&gt;.local/</code> over IPv6, or import a settings backup.</td></tr>
    <tr><td>Forgotten password</td><td>Forgot password on the sign-in page; an admin sets a temporary one.</td></tr>
    <tr><td>Lost phone with the authenticator</td><td>A recovery code instead of the app code, or an admin turns off 2FA (Users › Edit).</td></tr>
    <tr><td>New board</td><td>System › Settings › Settings backup: export (with passwords) and import on the new board.</td></tr>
    <tr><td>Maintenance: "the C6 firmware package is not on the gateway"</td><td>The C6 needs its large firmwares (first start, C6 update, reflash, ESPHome) and the package is neither in RAM nor downloadable: connect the gateway to the internet, or upload <code>c6fw.bin</code> of the release under System › Firmware and restart.</td></tr></tbody></table>""",
    """<table><thead><tr><th>Situace</th><th>Co dělat</th></tr></thead><tbody>
    <tr><td>Role nenaběhne</td><td>UI ukáže režim údržby s důvodem; zvolte jinou roli nebo příčinu odstraňte a restartujte.</td></tr>
    <tr><td>C6 neodpovídá</td><td>Zkontrolujte propojky JP1, Systém › Údržba › Přeflashovat C6.</td></tr>
    <tr><td>Vadná aktualizace brány</td><td>Automatický návrat do předchozího slotu; jinak <code>scripts/flash_p4.sh</code>.</td></tr>
    <tr><td>Pády</td><td>Systém › Diagnostika: core dump a log před posledním restartem.</td></tr>
    <tr><td>Nefunkční statická adresa</td><td>Nepotvrzená: po 120 s se vrátí sama. Potvrzená: <code>http://&lt;host&gt;.local/</code> přes IPv6, nebo import zálohy nastavení.</td></tr>
    <tr><td>Zapomenuté heslo</td><td>Zapomenuté heslo na přihlašovací stránce; správce nastaví dočasné.</td></tr>
    <tr><td>Ztracený telefon s Authenticatorem</td><td>Záložní kód místo kódu z aplikace, nebo správce vypne 2FA (Uživatelé › Upravit).</td></tr>
    <tr><td>Nová deska</td><td>Systém › Nastavení › Záloha nastavení: export (s hesly) a import na nové desce.</td></tr>
    <tr><td>Údržba: „the C6 firmware package is not on the gateway“</td><td>C6 potřebuje velké firmwary (první start, aktualizace C6, přeflashování, ESPHome) a balík není v RAM ani ke stažení: připojte bránu k internetu, nebo v Systém › Firmware nahrajte <code>c6fw.bin</code> z vydání a restartujte.</td></tr></tbody></table>""")

LABELS = {
    "title": T("P4 Gateway documentation", "Dokumentace P4 Gateway"),
    "intro": T("Introduction", "Úvod"), "hw": T("Hardware", "Hardware"), "install": T("First installation", "První instalace"),
    "ui": T("Web UI basics", "Základy webového rozhraní"), "pages": T("Web UI pages", "Stránky webového rozhraní"),
    "ha": T("Home Assistant", "Home Assistant"), "tg": T("Telegram bot", "Telegram bot"), "recovery": T("Troubleshooting", "Řešení problémů"),
    "howto": T("How to", "Postup"), "full": T("Whole page", "Celá stránka"), "shots": T("Screenshots", "Snímky obrazovky"),
    "zigbee_shot": T("in the Zigbee coordinator role", "v roli Zigbee koordinátor"), "router_shot": T("in the Zigbee router role", "v roli Zigbee router"),
    "dialogs": T("Dialogs", "Dialogy"),
    "foot": T("Screenshots: firmware v1.4, dark theme, English. The board, network names and addresses are those of the test setup.",
              "Snímky: firmware v1.4, tmavý vzhled, angličtina. Deska, názvy sítí a adresy jsou z testovací instalace."),
}

SECTION_ICONS = {"overview": "view-dashboard-outline", "role": "swap-horizontal", "thread": "thread", "zigbee": "zigbee", "sound": "waveform",
                 "ha": "home-assistant", "system": "developer-board", "profile": "account-circle-outline"}


# ---------------------------------------------------------------- rendering

def img(file: str, alt: str, cls: str = "shot") -> str:
    path = SHOTS / f"{file}.jpg"
    if not path.exists():
        return ""
    w, h = jpeg_size(path) or (0, 0)
    return (f'<a href="../screenshots/{file}.jpg" class="zoom"><img class="{cls}" src="../screenshots/{file}.jpg" '
            f'alt="{html.escape(alt)}" width="{w}" height="{h}" loading="lazy"></a>')


def suffix_label(key: str, lang: str) -> str:
    """Screenshots taken in another role than Thread border router say so."""
    if key.endswith("__zigbee"):
        return LABELS["zigbee_shot"][lang]
    if key.endswith("__router"):
        return LABELS["router_shot"][lang]
    return ""


def page_meta(key: str):
    return next((v for v in CARDS.values() if v["page"] == key), None)


def page_section(p, lang: str) -> str:
    route, icon, title, intro, cards, howto, shots = p
    out = [f'<section class="card wide" id="{route.replace("/", "-")}"><h3>{ic(icon)}<span>{title[lang]}</span></h3><p>{intro[lang]}</p>']
    used = set()
    for ctitle, desc in cards.items():
        # the first screenshot of this card among the page's screenshots
        pic = ""
        for key in shots:
            meta = page_meta(key)
            card = next((c for c in (meta["cards"] if meta else []) if re.sub(r"\s*\(\d+\)$", "", c["title"]) == ctitle and c["file"] not in used), None)
            if card:
                used.add(card["file"])
                note = suffix_label(key, lang)
                pic = img(card["file"], ctitle) + (f'<div class="cap">{note}</div>' if note else "")
                break
        out.append(f'<div class="item"><div class="item-text"><h4>{html.escape(card_name(ctitle, lang))}</h4><p>{desc[lang]}</p></div><div class="item-pic">{pic}</div></div>')
    if howto:
        out.append(f'<div class="howto"><h4>{ic("playlist-check")}{LABELS["howto"][lang]}</h4>{howto[lang]}</div>')
    fulls = []
    for key in shots:
        if page_meta(key):
            label = suffix_label(key, lang)
            fulls.append(f'<figure>{img(key, title[lang], "shot full")}<figcaption>{LABELS["full"][lang]}{" · " + label if label else ""}</figcaption></figure>')
    if fulls:
        out.append(f'<details><summary>{LABELS["full"][lang]} ({len(fulls)})</summary><div class="fulls">{"".join(fulls)}</div></details>')
    out.append("</section>")
    return "\n".join(out)


def build(lang: str) -> str:
    other = "cs" if lang == "en" else "en"
    L = lambda k: LABELS[k][lang]
    nav_items = [("intro", "information-outline", L("intro")), ("hw", "chip", L("hw")), ("install", "download", L("install")),
                 ("ui", "view-dashboard-outline", L("ui"))]
    page_nav = "".join(f'<a href="#{p[0].replace("/", "-")}" class="sub">{ic(p[1])}<span>{p[2][lang]}</span></a>' for p in PAGES)
    nav = "".join(f'<a href="#{i}">{ic(icn)}<span>{t}</span></a>' for i, icn, t in nav_items)
    nav += f'<a href="#pages">{ic("view-dashboard-outline")}<span>{L("pages")}</span></a><div class="subs">{page_nav}</div>'
    nav += "".join(f'<a href="#{i}">{ic(icn)}<span>{t}</span></a>' for i, icn, t in
                   [("ha", "home-assistant", L("ha")), ("tg", "send", L("tg")), ("recovery", "wrench-outline", L("recovery"))])
    tips = "".join(img(f, f) for f in ("tip_lan", "tip_role", "tip_peer", "tip_ha", "tip_internet", "tip_uptime"))
    dialogs = "".join(f'<figure>{img(f, f)}<figcaption>{c[lang]}</figcaption></figure>' for f, c in [
        ("login", T("Sign-in", "Přihlášení")), ("dialog_restart", T("Restart", "Restart")), ("dialog_new_user", T("New user", "Nový uživatel")),
        ("dialog_edit_user", T("Edit user with own permissions", "Úprava uživatele s vlastními oprávněními")),
        ("dialog_totp", T("Two-factor sign-in setup", "Nastavení dvoufázového přihlášení")),
        ("dialog_telegram_pair", T("Telegram pairing", "Párování Telegramu"))])
    flags = {"cs": '<svg class="flag" viewBox="0 0 6 4" preserveAspectRatio="none"><rect width="6" height="4" fill="#d7141a"/><rect width="6" height="2" fill="#fff"/><path d="M0 0 3 2 0 4z" fill="#11457e"/></svg>',
             "en": '<svg class="flag" viewBox="0 0 60 30" preserveAspectRatio="none"><clipPath id="fgb"><path d="M30 15h30v15zv15H0zH0V0zV0h30z"/></clipPath><path d="M0 0v30h60V0z" fill="#012169"/><path d="M0 0l60 30m0-30L0 30" stroke="#fff" stroke-width="6"/><path d="M0 0l60 30m0-30L0 30" clip-path="url(#fgb)" stroke="#c8102e" stroke-width="4"/><path d="M30 0v30M0 15h60" stroke="#fff" stroke-width="10"/><path d="M30 0v30M0 15h60" stroke="#c8102e" stroke-width="6"/></svg>'}
    return f"""<!doctype html>
<html lang="{lang}"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{L("title")}</title><link rel="icon" href="../assets/favicon.ico"><link rel="stylesheet" href="../assets/docs.css"></head>
<body>
<header><div class="bar"><button id="menu-btn" aria-label="Menu">{ic("menu")}</button>
  <div class="brand">{ic("access-point-network")}P4 Gateway <small>{L("title")}</small></div><div class="spacer"></div>
  <span class="seg"><a class="{'on' if lang == 'cs' else ''}" href="../cs/index.html" title="Čeština">{flags['cs']}</a><a class="{'on' if lang == 'en' else ''}" href="../en/index.html" title="English">{flags['en']}</a></span>
</div></header>
<div class="layout"><nav id="side">{nav}</nav><div id="scrim"></div>
<main>
<h2 class="page-title">{ic("book-open-variant")}<span>{L("title")}</span></h2>
<section class="card wide" id="intro"><h3>{ic("information-outline")}<span>{L("intro")}</span></h3>{INTRO[lang]}{ROLES_TABLE[lang]}</section>
<section class="card wide" id="hw"><h3>{ic("chip")}<span>{L("hw")}</span></h3>{HW[lang]}</section>
<section class="card wide" id="install"><h3>{ic("download")}<span>{L("install")}</span></h3>{INSTALL[lang]}</section>
<section class="card wide" id="ui"><h3>{ic("view-dashboard-outline")}<span>{L("ui")}</span></h3>{UI_BASICS[lang]}<div class="tips">{tips}</div>
  <h4>{L("dialogs")}</h4><div class="fulls">{dialogs}</div></section>
<h2 class="page-title" id="pages">{ic("view-dashboard-outline")}<span>{L("pages")}</span></h2>
{"".join(page_section(p, lang) for p in PAGES)}
<section class="card wide" id="ha"><h3>{ic("home-assistant")}<span>{L("ha")}</span></h3>{HA[lang]}</section>
<section class="card wide" id="tg"><h3>{ic("send")}<span>{L("tg")}</span></h3>{TELEGRAM[lang]}</section>
<section class="card wide" id="recovery"><h3>{ic("wrench-outline")}<span>{L("recovery")}</span></h3>{RECOVERY[lang]}</section>
<p class="muted foot">{L("foot")}</p>
</main></div>
<div id="lightbox" hidden><img alt=""></div>
<script>
document.addEventListener('click', e => {{
  const a = e.target.closest('a.zoom');
  const lb = document.getElementById('lightbox');
  if (a) {{ e.preventDefault(); lb.querySelector('img').src = a.href; lb.hidden = false; return; }}
  if (e.target.closest('#lightbox')) lb.hidden = true;
  if (e.target.closest('#menu-btn')) document.body.classList.toggle('menu-open');
  else if (e.target.closest('#side a') || e.target.closest('#scrim')) document.body.classList.remove('menu-open');
}});
document.addEventListener('keydown', e => {{ if (e.key === 'Escape') document.getElementById('lightbox').hidden = true; }});
</script>
</body></html>
"""


def main() -> None:
    for lang in ("en", "cs"):
        (OUT / lang).mkdir(parents=True, exist_ok=True)
        (OUT / lang / "index.html").write_text(build(lang))
    print("written:", ", ".join(str((OUT / l / "index.html").relative_to(ROOT)) for l in ("en", "cs")))


if __name__ == "__main__":
    main()
