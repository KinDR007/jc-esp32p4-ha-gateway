#!/usr/bin/env python3
"""Builds marketing/index.html, the project page: the root of the GitHub Pages site (.github/workflows/pages.yml),
the user documentation under docs/.
Texts in Czech and English side by side (T), icons are the MDI paths of the web UI
(firmware/gateway/web/index.html), screenshots copied from documentation/screenshots into img/.
Usage: python3 marketing/build.py"""
import json
import re
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
GH = 'https://github.com/KinDR007/jc-esp32p4-ha-gateway'
SITE = 'https://kindr007.github.io/jc-esp32p4-ha-gateway/'
DOCS = SITE + 'docs/'
BLOB = GH + '/blob/main/'

ui = (ROOT / 'firmware/gateway/web/index.html').read_text()
block = ui[ui.index('const ICONS = {'):]
block = block[:block.index('};')]
ICONS = dict(re.findall(r"'([a-z0-9-]+)':\s*'([^']+)'", block))


def doc(anchor=''):
    """attributes of a link into the HTML user documentation: Czech by default, app.js follows the page language"""
    return f'href="{DOCS}cs/index.html{"#" + anchor if anchor else ""}" data-doc="{anchor}"'


def ic(name):
    return f'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="{ICONS[name]}"/></svg>'


def T(cs, en, tag='span'):
    return f'<{tag} data-l="cs">{cs}</{tag}><{tag} data-l="en">{en}</{tag}>'


def card(icon, title, text, link=None):
    more = f'<a class="more" href="{link}" target="_blank" rel="noopener">{T("Dokumentace →", "Documentation →")}</a>' if link else ''
    return f'<div class="card reveal"><div class="ico">{ic(icon)}</div><h3>{title}</h3><p>{text}</p>{more}</div>'


BENEFITS = [
    ('shield-lock-outline', T('Vše lokálně, bez cloudu', 'Local, no cloud'),
     T('Vše běží na desce ve vaší síti. Žádný účet u výrobce, žádné předplatné, data zůstávají doma.',
       'Everything runs on the board in your network. No vendor account, no subscription, data stays at home.')),
    ('swap-horizontal', T('Dvě rádia současně', 'Two radios at once'),
     T('Thread i Zigbee najednou: rádio A je přímo na desce, jako rádio B poslouží volitelný modul XIAO ESP32-C5. Role rádií přepnete jedním kliknutím.',
       'Thread and Zigbee at the same time: radio A on the board and an optional XIAO ESP32-C5 as radio B. Switch roles in one click.')),
    ('home-assistant', T('Hotová integrace pro Home Assistant', 'Ready Home Assistant integration'),
     T('Vlastní integrace z HACS, MQTT broker není potřeba. Zařízení, senzory i piny se v Home Assistant objeví automaticky.',
       'A dedicated integration from HACS, no MQTT broker needed. Devices, sensors and pins appear in Home Assistant automatically.')),
    ('developer-board', T('Jedna cenově dostupná deska', 'One affordable board'),
     T('Deska Guition JC-ESP32P4-M3-DEV s Ethernetem nahradí hned několik USB adaptérů i jednodeskový počítač.',
       'A Guition JC-ESP32P4-M3-DEV with Ethernet replaces several USB dongles and a single-board computer.')),
    ('cloud-download-outline', T('Aktualizace z prohlížeče', 'Updates from the browser'),
     T('Nové verze nainstalujete přímo z GitHubu, v případě potřeby se vrátíte ke starší. Nastavení snadno zazálohujete.',
       'Install new releases straight from GitHub, roll back when needed and back up your settings.')),
    ('account-multiple-outline', T('Přístup pod kontrolou', 'Access under control'),
     T('Uživatelské účty s rolemi a oprávněními pro jednotlivé stránky, dvoufázové ověření a API tokeny.',
       'User accounts with roles and per-page permissions, two-factor authentication and API tokens.')),
]

USES = [
    ('matter', 'Thread & Matter',
     T('Border router pro zařízení Matter', 'Border router for Matter devices'),
     T('Brána plní roli plnohodnotného OpenThread Border Routeru. Home Assistant se k ní připojí integrací OTBR a zařízení Matter pak párujete přímo z telefonu.',
       'The gateway is a full-featured OpenThread Border Router. Home Assistant connects to it through the OTBR integration, and you pair Matter devices right from your phone.'),
     [T('Mapa sítě Thread včetně kvality spojení', 'Thread network map with link quality'),
      T('Připojení k existující síti pomocí datasetu', 'Join an existing network with a dataset'),
      T('Režim routeru pro rozšíření dosahu sítě', 'Router mode to extend the mesh')],
     'thread_topology__1.jpg', 'ha'),
    ('zigbee', 'Zigbee',
     T('Koordinátor pro Zigbee2MQTT', 'Coordinator for Zigbee2MQTT'),
     T('Síťový koordinátor Zigbee dostupný přes TCP (port 6638), nebo router, který rozšíří dosah vaší sítě Zigbee. Síť se zálohuje automaticky.',
       'A network Zigbee coordinator over TCP (port 6638), or a router that extends the range of your Zigbee network. The network is backed up automatically.'),
     [T('Mapa sítě a přehled zařízení ze Zigbee2MQTT', 'Network map and devices from Zigbee2MQTT'),
      T('Povolení párování přímo z webu', 'Permit joining from the web'),
      T('Automatické noční zálohy', 'Automatic nightly backups')],
     'zigbee_network__1.jpg', 'zigbee-network'),
    ('serial-port', T('Energie', 'Energy'),
     T('Elektroměry, střídače a Modbus', 'Meters, inverters and Modbus'),
     T('Zařízení na RS485 i Modbus TCP připojíte pomocí hotových šablon pro Eastron, ABB, Chint, ORNO, Peacefair, Victron, GoodWe a Huawei. Brána je může zpřístupnit dalším systémům jako server Modbus TCP, nebo jeden elektroměr vydávat za jiný.',
       'Connect RS485 and Modbus TCP devices with ready-made templates for Eastron, ABB, Chint, ORNO, Peacefair, Victron, GoodWe and Huawei. The gateway can share them with other systems as a Modbus TCP server, or present one meter as another.'),
     [T('Vyhledání zařízení na sběrnici i v síti', 'Device discovery on the bus and in the network'),
      T('Emulace: SDM630 se střídači představí jako ABB B23', 'Emulation: an SDM630 appears to your inverter as an ABB B23'),
      T('Vlastní šablony bez programování', 'Custom templates without coding')],
     'system_rs485__3.jpg', 'system-rs485'),
    ('ear-hearing', T('Zvuk', 'Sound'),
     T('Rozpozná kouřový hlásič i tříštění skla', 'Recognises smoke alarms and breaking glass'),
     T('Vestavěný mikrofon a model YAMNet běžící přímo na čipu rozpoznají 521 druhů zvuků. Bránu naučíte i vlastní zvuky a změří vám hladinu hluku v dB(A).',
       'The built-in microphone and the YAMNet model running on the chip recognise 521 sound classes. You can also teach it your own sounds, and it measures noise levels in dB(A).'),
     [T('Události a binární senzory v Home Assistant', 'Events and binary sensors in Home Assistant'),
      T('Zvonek, štěkání psa, poplach, siréna', 'Doorbell, barking, alarms, sirens'),
      T('Zpracování probíhá offline, nic se neodesílá do cloudu', 'Processed offline, nothing goes to the cloud')],
     'sound_live.jpg', 'sound-live'),
    ('chip', T('Pro kutily', 'For makers'),
     T('Senzory, relé, LED pásky a impulzní měřiče', 'Sensors, relays, LED strips and pulse meters'),
     T('24 ovladačů senzorů I2C (teplota, vlhkost, CO2, prašnost, osvětlení) a volné piny konektoru JP1, které poslouží jako vstupy, výstupy, PWM, čítače impulzů z vodoměru, teploměry 1-Wire, LED pásek WS2812 nebo most UART-TCP.',
       '24 I2C sensor drivers (temperature, humidity, CO2, particulate matter, light) and the free pins of header JP1 as inputs, outputs, PWM, water meter pulse counters, 1-Wire thermometers, a WS2812 LED strip or a UART-TCP bridge.'),
     [T('Vše dostupné jako entity v Home Assistant', 'Everything available as Home Assistant entities'),
      T('Stav po restartu lze nastavit pro každý pin', 'Power-on state configurable per pin'),
      T('Druhá sběrnice I2C na JP1', 'A second I2C bus on JP1')],
     'system_gpio__1.jpg', 'system-gpio'),
    ('send', 'Telegram',
     T('Upozornění a pravidelné přehledy', 'Alerts and periodic summaries'),
     T('Bot posílá upozornění a přijímá příkazy. Každý uživatel si nastaví vlastní pravidelné přehledy: aktuální hodnoty, minima, maxima, průměry a spotřebu energie za den či týden.',
       'The bot sends alerts and takes commands. Each user can schedule their own reports: current values, minimum, maximum, average and daily or weekly energy use.'),
     [T('Tiché hodiny a úroveň upozornění podle tématu', 'Quiet hours and alert levels per topic'),
      T('Příkazy s potvrzením a dvoufázovým ověřením', 'Commands with confirmation and two-factor authentication'),
      T('Přehledy ze zařízení Modbus, senzorů a GPIO', 'Reports on Modbus devices, sensors and GPIO')],
     'profile__4.jpg', 'profile'),
]

# donations: support.json next to this script (kept out of git with the folder); the section shows only what is filled in
SUPPORT = {'github': '', 'paypal': '', 'paypal_qr': '', 'iban': '', 'account': '', 'holder': '', 'message': 'P4 Gateway'}
try:
    SUPPORT.update(json.loads((HERE / 'support.json').read_text()))
except FileNotFoundError:
    pass


def qr_platba(iban, message):
    """Czech QR payment (SPD 1.0, cbaonline.cz) as an SVG in img/; None without the qrcode module"""
    try:
        import qrcode
        import qrcode.image.svg
    except ImportError:
        return None
    msg = unicodedata.normalize('NFKD', message).encode('ascii', 'ignore').decode().replace('*', ' ')[:60]
    spd = f'SPD*1.0*ACC:{iban.replace(" ", "").upper()}*CC:CZK*MSG:{msg.upper()}'
    img = qrcode.make(spd, image_factory=qrcode.image.svg.SvgPathImage, box_size=10, border=2,
                      error_correction=qrcode.constants.ERROR_CORRECT_M)
    img.save(str(HERE / 'img/qr_platba.svg'))
    return 'img/qr_platba.svg'


def support():
    paypal, iban, account = SUPPORT['paypal'].strip(), SUPPORT['iban'].strip(), SUPPORT['account'].strip()
    github = SUPPORT['github'].strip()
    if not (paypal or iban or account or github):
        return ''
    cards = []
    if github:
        cards.append(f'''<div class="card reveal give"><div class="ico">{ic('account-multiple-outline')}</div><h3>GitHub Sponsors</h3>
          <p>{T('Jednorázová i pravidelná měsíční podpora přímo na GitHubu, bez poplatků pro autora.', 'One-time or monthly support right on GitHub, with no fees for the author.')}</p>
          <a class="btn primary" href="https://github.com/sponsors/{github}" target="_blank" rel="noopener">{T('Sponzorovat na GitHubu', 'Sponsor on GitHub')}</a></div>''')
    if paypal:
        url = paypal if paypal.startswith('http') else f'https://www.paypal.com/paypalme/{paypal}'
        qr = SUPPORT['paypal_qr'].strip()
        pp_qr = f'<figure class="qr"><img src="{qr}" alt="PayPal QR"><figcaption>{T("Načtěte fotoaparátem telefonu", "Scan with your phone camera")}</figcaption></figure>' if qr else ''
        cards.append(f'''<div class="card reveal give"><div class="ico">{ic('send')}</div><h3>PayPal</h3>
          <p>{T('Rychlý příspěvek kartou nebo z účtu PayPal, částku zvolíte sami.', 'A quick gift by card or from your PayPal account, any amount you like.')}</p>
          <div class="bank"><a class="btn primary" href="{url}" target="_blank" rel="noopener">{T('Přispět přes PayPal', 'Donate with PayPal')}</a>{pp_qr}</div></div>''')
    if iban or account:
        qr = qr_platba(iban, SUPPORT['message']) if iban else None
        rows = ''.join(f'<tr><td>{k}</td><td><code>{v}</code></td></tr>' for k, v in (
            (T('Číslo účtu', 'Account number'), account), ('IBAN', iban), (T('Majitel', 'Account holder'), SUPPORT['holder']),
            (T('Zpráva pro příjemce', 'Payment reference'), SUPPORT['message'])) if v)
        qr_html = f'''<figure class="qr"><img src="{qr}" alt="QR platba"><figcaption>{T('Načtěte v bankovní aplikaci (QR platba)', 'Scan in a Czech banking app (QR payment)')}</figcaption></figure>''' if qr else ''
        cards.append(f'''<div class="card reveal give"><div class="ico">{ic('qrcode')}</div><h3>{T('Bankovní převod', 'Bank transfer')}</h3>
          <p>{T('Převodem na účet, jednorázově i trvalým příkazem.', 'By bank transfer, once or as a standing order.')}</p>
          <div class="bank"><table class="spec"><tbody>{rows}</tbody></table>{qr_html}</div></div>''')
    return f'''<section class="alt" id="support"><div class="wrap">
  <div class="head reveal"><div class="kicker">{T('Podpořte vývoj', 'Support the project')}</div>
    <h2>{T('Líbí se vám P4 Gateway?', 'Do you like P4 Gateway?')}</h2>
    <p>{T('Projekt je zdarma a s otevřeným kódem a vyvíjím ho ve volném čase. Příspěvek pomůže pokrýt hardware na testování (rádia, elektroměry, střídače) a čas na nové funkce. Děkuji!',
      'The project is free and open source, and I develop it in my spare time. A contribution helps cover test hardware (radios, meters, inverters) and time for new features. Thank you!')}</p></div>
  <div class="grid support-grid{' one' if len(cards) == 1 else ' three' if len(cards) == 3 else ''}">{''.join(cards)}</div>
</div></section>'''


GALLERY = [
    ('overview.jpg', T('Přehled', 'Overview'), T('Stav brány, rádií a sítí na první pohled.', 'The state of the gateway, radios and networks at a glance.')),
    ('flows.jpg', T('Toky dat', 'Data flows'), T('Kudy proudí data jednotlivých sítí a služeb.', 'How data flows between networks and services.')),
    ('radios_roles.jpg', T('Role rádií', 'Radio roles'), T('Přiřazení rolí rádiím, přepnutí jedním kliknutím.', 'Assign a role to each radio and switch in one click.')),
    ('radios_scan__2.jpg', T('Rádiové okolí', 'Radio surroundings'), T('Průzkum pásma 2,4 GHz s doporučením vhodného kanálu.', 'A 2.4 GHz band scan with a recommended channel.')),
    ('system_mbserver.jpg', T('Server Modbus TCP', 'Modbus TCP server'), T('Zpřístupnění zařízení dalším systémům v síti.', 'Share devices with other systems on your network.')),
    ('system_hw.jpg', T('Hardware', 'Hardware'), T('Senzory I2C, informace o desce a zapojení.', 'I2C sensors, the board and its wiring.')),
]

DOCS_LINKS = [
    (T('Uživatelská dokumentace', 'User documentation'), T('Popis všech stránek rozhraní se snímky obrazovky, česky i anglicky.', 'Every page of the web interface with screenshots, in Czech and English.'), ''),
    (T('Instalace', 'Installation'), T('Sestavení, první instalace, aktualizace a obnova.', 'Building, first installation, updates and recovery.'), 'install'),
    (T('Hardware', 'Hardware'), T('Deska, propojky JP1, rozložení pinů a rádio B.', 'The board, JP1 jumpers, pinout and radio B.'), 'hw'),
    ('Home Assistant', T('Integrace, OTBR, Matter, Zigbee2MQTT, MQTT.', 'Integration, OTBR, Matter, Zigbee2MQTT, MQTT.'), 'ha'),
    ('Modbus', T('Šablony, hledání, server, emulace.', 'Templates, search, server, emulation.'), 'system-rs485'),
    ('GPIO', T('Vstupy, výstupy, PWM, čítače, 1-Wire, WS2812.', 'Inputs, outputs, PWM, counters, 1-Wire, WS2812.'), 'system-gpio'),
    (T('Notifikace a Telegram', 'Notifications and Telegram'), T('Bot, příkazy, periodické zprávy.', 'Bot, commands, periodic reports.'), 'tg'),
    ('HTTP API', T('128 endpointů s ukázkami volání.', '128 endpoints with request examples.'), 'api'),
]

STATS = [('2', T('rádia současně', 'simultaneous radios')), ('5', T('rolí rádií', 'radio roles')), ('521', T('druhů zvuků', 'sound classes')),
         ('18', T('šablon Modbus', 'Modbus templates')), ('24', T('ovladačů senzorů', 'sensor drivers')), ('128', T('endpointů API', 'API endpoints'))]

# architecture diagram: sources -> board -> consumers
LEFT = [('Thread / Matter', T('světla, senzory', 'lights, sensors')), ('Zigbee', T('vypínače, čidla', 'switches, sensors')),
        ('Bluetooth', T('ESPHome proxy', 'ESPHome proxy')), ('RS485 / Modbus', T('elektroměry, střídače', 'meters, inverters')),
        ('I2C · GPIO', T('senzory, relé, LED', 'sensors, relays, LEDs')), (T('Mikrofon', 'Microphone'), T('zvukové události', 'sound events'))]
RIGHT = [('Home Assistant', T('integrace P4 Gateway', 'P4 Gateway integration')), ('Zigbee2MQTT', 'TCP 6638'),
         ('MQTT', T('discovery', 'discovery')), ('Telegram', T('upozornění, přehledy', 'alerts, reports')),
         (T('Klienti Modbus TCP', 'Modbus TCP clients'), T('Victron, SCADA, PLC', 'Victron, SCADA, PLC')), ('REST API', T('skripty, automatizace', 'scripts, automations'))]


def svg_text(x, y, content, cls='', anchor='start'):
    """a text that may hold two languages (T spans become tspans)"""
    parts = re.findall(r'<span data-l="(cs|en)">(.*?)</span>', content)
    c = f' class="{cls}"' if cls else ''
    if not parts:
        return f'<text x="{x}" y="{y}" text-anchor="{anchor}"{c}>{content}</text>'
    return ''.join(f'<text x="{x}" y="{y}" text-anchor="{anchor}" data-l="{l}"{c}>{t}</text>' for l, t in parts)


def arch():
    w, h, rowh, top = 1100, 520, 78, 34
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="architecture">',
           '<defs><linearGradient id="core" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#5b74fb"/><stop offset="1" stop-color="#9a5cf6"/></linearGradient>'
           '<linearGradient id="line" x1="0" x2="1"><stop offset="0" stop-color="#5b74fb"/><stop offset="1" stop-color="#9a5cf6"/></linearGradient></defs>']
    cx, cy, cw, ch = 400, 110, 300, 300
    for i, (a, b) in enumerate(LEFT):
        y = top + i * rowh
        out.append(f'<rect class="box" x="10" y="{y}" width="250" height="60" rx="12"/>')
        out.append(svg_text(28, y + 26, a))
        out.append(svg_text(28, y + 46, b, 'sub'))
        out.append(f'<path class="flow" d="M260 {y + 30} C 330 {y + 30}, 330 {cy + ch / 2}, {cx} {cy + ch / 2}"/>')
    for i, (a, b) in enumerate(RIGHT):
        y = top + i * rowh
        out.append(f'<rect class="box" x="{w - 260}" y="{y}" width="250" height="60" rx="12"/>')
        out.append(svg_text(w - 242, y + 26, a))
        out.append(svg_text(w - 242, y + 46, b, 'sub'))
        out.append(f'<path class="flow" d="M{cx + cw} {cy + ch / 2} C {cx + cw + 70} {cy + ch / 2}, {w - 330} {y + 30}, {w - 260} {y + 30}"/>')
    out.append(f'<rect class="core" x="{cx}" y="{cy}" width="{cw}" height="{ch}" rx="28"/>')
    out.append(f'<text x="{cx + cw / 2}" y="{cy + 52}" text-anchor="middle" style="fill:#fff;font-size:22px;font-weight:800">P4 Gateway</text>')
    out.append(f'<text x="{cx + cw / 2}" y="{cy + 76}" text-anchor="middle" style="fill:rgba(255,255,255,.8);font-size:13px;font-weight:400">ESP32-P4 · Ethernet</text>')
    for k, (a, b) in enumerate((('Radio A', 'ESP32-C6'), ('Radio B', 'XIAO ESP32-C5'))):
        y = cy + 108 + k * 86
        out.append(f'<rect x="{cx + 28}" y="{y}" width="{cw - 56}" height="66" rx="14" style="fill:rgba(255,255,255,.16);stroke:rgba(255,255,255,.35)"/>')
        out.append(f'<text x="{cx + cw / 2}" y="{y + 28}" text-anchor="middle" style="fill:#fff">{a}</text>')
        out.append(f'<text x="{cx + cw / 2}" y="{y + 48}" text-anchor="middle" style="fill:rgba(255,255,255,.8);font-size:12px;font-weight:400">{b}</text>')
    out.append('</svg>')
    return ''.join(out)


def use_case(u):
    icon, tag, title, text, items, img, anchor = u
    lis = ''.join(f'<li>{x}</li>' for x in items)
    return f'''<div class="uc reveal"><div><span class="tag">{tag}</span><h3>{title}</h3><p>{text}</p><ul>{lis}</ul>
      <a class="more" style="display:inline-block;margin-top:14px;color:var(--a1);font-weight:600;text-decoration:none" {doc(anchor)} target="_blank" rel="noopener">{T("Jak na to →", "How to →")}</a></div>
      <div class="uc-img"><div class="frame"><div class="dots"><i></i><i></i><i></i></div><img src="img/{img}" alt=""></div></div></div>'''


html = f'''<!doctype html>
<html lang="cs" data-lang="cs">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>P4 Gateway: Thread, Zigbee, Matter a Modbus pro Home Assistant</title>
<meta name="description" content="Open-source síťová brána pro Home Assistant na desce ESP32-P4: Thread border router, koordinátor Zigbee, Modbus, rozpoznávání zvuků, GPIO a Telegram.">
<link rel="icon" href="img/p4gw-small.svg">
<link rel="stylesheet" href="style.css">
<script>try {{ const l = localStorage.getItem('lang'), t = localStorage.getItem('theme');
  document.documentElement.dataset.lang = l || (navigator.language.startsWith('cs') ? 'cs' : 'en'); if (t) document.documentElement.dataset.theme = t; }} catch (e) {{}}</script>
</head>
<body data-docs="{DOCS}">
<header><div class="wrap bar">
  <a class="logo" href="#top"><img src="img/p4gw.svg" alt="">P4 Gateway</a>
  <nav><a href="#why">{T('Proč', 'Why')}</a><a href="#how">{T('Jak to funguje', 'How it works')}</a><a href="#uses">{T('Co vytvoříte', 'Use cases')}</a>
    <a href="#gallery">{T('Ukázky', 'Screens')}</a><a href="#start">{T('Začít', 'Get started')}</a><a href="#docs">{T('Dokumentace', 'Docs')}</a>{f'<a href="#support">{T("Podpořit", "Support")}</a>' if support() else ''}</nav>
  <div class="tools"><button id="lang" title="Language">{T('EN', 'CZ')}</button><button id="theme" title="Theme">◐</button></div>
</div></header>

<section class="hero" id="top"><div class="wrap hero-grid">
  <div>
    <span class="eyebrow"><b>●</b> {T('Open source · běží lokálně · pro Home Assistant', 'Open source · runs locally · for Home Assistant')}</span>
    <h1>{T('Jedna deska.<br><span class="grad">Celý chytrý dům.</span>', 'One board.<br><span class="grad">Your whole smart home.</span>')}</h1>
    <p class="lead">{T('Thread a Matter, Zigbee, Bluetooth proxy, elektroměry a střídače na Modbusu, senzory, relé i rozpoznávání zvuků. Vše na jedné desce ESP32-P4 ve vaší síti, nastavené v prohlížeči a ihned dostupné v Home Assistant.',
      'Thread and Matter, Zigbee, a Bluetooth proxy, meters and inverters on Modbus, sensors, relays and sound recognition. All on one ESP32-P4 board on your network, configured in the browser and instantly available in Home Assistant.')}</p>
    <div class="cta"><a class="btn primary" {doc()} target="_blank" rel="noopener">{ic('book-open-variant')}{T('Dokumentace', 'Documentation')}</a>
      <a class="btn" href="{GH}" target="_blank" rel="noopener">{ic('developer-board')}GitHub</a>
      <a class="btn" href="https://my.home-assistant.io/redirect/hacs_repository/?owner=KinDR007&repository=jc-esp32p4-ha-gateway&category=integration" target="_blank" rel="noopener">{ic('home-assistant')}{T('Přidat do HACS', 'Add to HACS')}</a></div>
    <div class="chips"><span class="chip">Thread BR</span><span class="chip">Matter</span><span class="chip">Zigbee2MQTT</span><span class="chip">ESPHome BLE</span>
      <span class="chip">Modbus RTU/TCP</span><span class="chip">MQTT</span><span class="chip">Telegram</span><span class="chip">REST API</span></div>
  </div>
  <div class="frame"><div class="dots"><i></i><i></i><i></i></div><img src="img/overview.jpg" alt="P4 Gateway web UI"></div>
</div>
<div class="wrap"><div class="stats">{''.join(f'<div class="stat reveal"><b class="grad">{n}</b><span>{t}</span></div>' for n, t in STATS)}</div></div>
</section>

<section class="alt" id="why"><div class="wrap">
  <div class="head reveal"><div class="kicker">{T('Proč P4 Gateway', 'Why P4 Gateway')}</div>
    <h2>{T('Méně krabiček, víc možností', 'Fewer boxes, more possibilities')}</h2>
    <p>{T('Jedna úsporná deska, která se o sebe postará sama, místo několika USB adaptérů, Raspberry Pi a cloudových aplikací.', 'One efficient, self-maintaining board instead of several USB dongles, a Raspberry Pi and cloud apps.')}</p></div>
  <div class="grid g3">{''.join(card(*b) for b in BENEFITS)}</div>
</div></section>

<section id="how"><div class="wrap">
  <div class="head reveal"><div class="kicker">{T('Jak to funguje', 'How it works')}</div>
    <h2>{T('Zařízení vlevo, váš systém vpravo', 'Devices on the left, your system on the right')}</h2>
    <p>{T('Brána sbírá data z rádiových sítí, sběrnic i vlastních čidel a předává je tam, kde je potřebujete. Každé rádio plní jednu roli, kterou lze kdykoli změnit.',
      'The gateway collects data from radio networks, buses and its own sensors and passes it where you need it. Each radio has a single role that you can change at any time.')}</p></div>
  <div class="arch reveal">{arch()}</div>
</div></section>

<section class="alt" id="uses"><div class="wrap">
  <div class="head reveal"><div class="kicker">{T('Co vytvoříte', 'What you can build')}</div>
    <h2>{T('Od světel Matter po regulaci nulového přetoku', 'From Matter lighting to zero-export control')}</h2>
    <p>{T('Několik příkladů toho, co brána zvládne hned po nastavení v prohlížeči. Bez programování.', 'A few examples of what the gateway can do as soon as it is set up in the browser. No coding required.')}</p></div>
  {''.join(use_case(u) for u in USES)}
</div></section>

<section id="gallery"><div class="wrap">
  <div class="head reveal"><div class="kicker">{T('Webové rozhraní', 'Web interface')}</div>
    <h2>{T('Přehledné rozhraní v češtině', 'A clear interface in English and Czech')}</h2>
    <p>{T('Vše nastavíte v prohlížeči na počítači i v mobilu. Tmavý i světlý vzhled a nápověda u každé karty.', 'Configure everything in the browser on desktop or mobile, with dark and light themes and help on every card.')}</p></div>
  <div class="tabs" id="tabs">{''.join(f'<button data-i="{i}" class="{"on" if i == 0 else ""}">{t}</button>' for i, (_, t, _) in enumerate(GALLERY))}</div>
  <figure class="frame shot" style="margin:0"><div class="dots"><i></i><i></i><i></i></div><img id="shot" src="img/{GALLERY[0][0]}" alt=""><figcaption id="cap">{GALLERY[0][2]}</figcaption></figure>
  <template id="shots">{''.join(f'<div data-src="img/{f}">{c}</div>' for f, _, c in GALLERY)}</template>
</div></section>

<section class="alt" id="hw"><div class="wrap hw">
  <div class="reveal"><div class="head" style="margin-bottom:20px"><div class="kicker">Hardware</div><h2>{T('Postaveno na ESP32-P4', 'Built on the ESP32-P4')}</h2>
    <p>{T('Deska Guition JC-ESP32P4-M3-DEV s Ethernetem a rádiem ESP32-C6. Druhé rádio, XIAO ESP32-C5, připojíte ke konektoru JP1.', 'A Guition JC-ESP32P4-M3-DEV board with Ethernet and an ESP32-C6 radio. A second XIAO ESP32-C5 radio plugs into header JP1.')}</p></div>
    <table class="spec"><tbody>
      <tr><td>{T('Procesor', 'Processor')}</td><td>ESP32-P4, 32 MB PSRAM</td></tr>
      <tr><td>{T('Síť', 'Network')}</td><td>{T('Ethernet 100 Mbit/s, záložní připojení přes Wi-Fi rádia', '100 Mbit/s Ethernet, Wi-Fi backup through a radio')}</td></tr>
      <tr><td>{T('Rádio A', 'Radio A')}</td><td>ESP32-C6: Thread, Zigbee, Bluetooth</td></tr>
      <tr><td>{T('Rádio B (volitelné)', 'Radio B (optional)')}</td><td>XIAO ESP32-C5: Thread, Zigbee, Bluetooth, {T('Wi-Fi 5 GHz', '5 GHz Wi-Fi')}</td></tr>
      <tr><td>{T('Rozhraní', 'Interfaces')}</td><td>RS485 (J4), I2C (CN3), GPIO JP1, {T('mikrofon', 'microphone')}</td></tr>
      <tr><td>{T('Ověřeno', 'Tested')}</td><td>{T('57 automatických testů hardwaru a 39 testů rozhraní', '57 automated hardware tests and 39 UI tests')}</td></tr>
    </tbody></table>
    <a class="more" style="display:inline-block;margin-top:16px;color:var(--a1);font-weight:600;text-decoration:none" {doc('hw')} target="_blank" rel="noopener">{T('Zapojení a propojky →', 'Wiring and jumpers →')}</a></div>
  <div class="frame reveal"><div class="dots"><i></i><i></i><i></i></div><img src="img/radios_roles.jpg" alt=""></div>
</div></section>

<section id="start"><div class="wrap">
  <div class="head reveal"><div class="kicker">{T('Začněte', 'Get started')}</div><h2>{T('Tři kroky ke spuštění', 'Three steps to a working gateway')}</h2></div>
  <div class="grid g3 steps">
    <div class="card step reveal"><h3>{T('Nahrajte firmware', 'Flash the firmware')}</h3><p>{T('Stáhněte nejnovější vydání z GitHubu a nahrajte ho přes USB. Další aktualizace už nainstalujete z prohlížeče.', 'Download the latest release from GitHub and flash it over USB. Future updates install from the browser.')}</p></div>
    <div class="card step reveal"><h3>{T('Zvolte role rádií', 'Choose the radio roles')}</h3><p>{T('Otevřete <code>http://p4gw-xxxxxx.local</code>, zvolte Thread, Zigbee nebo Bluetooth a přidejte zařízení Modbus či senzory.', 'Open <code>http://p4gw-xxxxxx.local</code>, pick Thread, Zigbee or Bluetooth and add Modbus devices or sensors.')}</p></div>
    <div class="card step reveal"><h3>{T('Připojte Home Assistant', 'Connect Home Assistant')}</h3><p>{T('Integraci P4 Gateway nainstalujete z HACS a Home Assistant bránu v síti najde sám.', 'Install the P4 Gateway integration from HACS and Home Assistant finds the gateway on its own.')}</p></div>
  </div>
</div></section>

<section class="alt docs" id="docs"><div class="wrap">
  <div class="head reveal"><div class="kicker">{T('Dokumentace', 'Documentation')}</div><h2>{T('Vše srozumitelně popsáno, krok za krokem', 'Everything explained, step by step')}</h2></div>
  <div class="grid g4">{''.join(f'<a class="card reveal" {doc(a)} target="_blank" rel="noopener"><h3>{t}</h3><p>{d}</p></a>' for t, d, a in DOCS_LINKS)}</div>
</div></section>

{support()}

<section><div class="wrap"><div class="final reveal">
  <h2>{T('Brána, která patří jen vám', 'A gateway that is truly yours')}</h2>
  <p>{T('Otevřený kód, žádný cloud, žádné předplatné. Vyzkoušejte ji ještě dnes.', 'Open source, no cloud, no subscription. Try it today.')}</p>
  <div class="cta" style="justify-content:center"><a class="btn" href="{GH}/releases/latest" target="_blank" rel="noopener">{ic('cloud-download-outline')}{T('Stáhnout nejnovější verzi', 'Download the latest release')}</a>
    <a class="btn ghost" {doc()} target="_blank" rel="noopener">{ic('book-open-variant')}{T('Číst dokumentaci', 'Read the docs')}</a>
    {f'<a class="btn ghost" href="#support">♥ {T("Podpořit projekt", "Support the project")}</a>' if support() else ''}</div>
</div></div></section>

<footer><div class="wrap"><span>P4 Gateway · <a href="{GH}" target="_blank" rel="noopener">GitHub</a> · <a {doc()} target="_blank" rel="noopener">{T('Dokumentace', 'Documentation')}</a> · <a href="{GH}/releases" target="_blank" rel="noopener">{T('Vydání', 'Releases')}</a></span>
  <span>{T('Ikony: Material Design Icons (Apache-2.0)', 'Icons: Material Design Icons (Apache-2.0)')}</span></div></footer>

<script src="app.js"></script>
</body>
</html>
'''
(HERE / 'index.html').write_text(html)
print('written', HERE / 'index.html', len(html), 'bytes')
