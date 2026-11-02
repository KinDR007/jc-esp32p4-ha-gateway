"""Before/after images for README.md: the whole menu (each section opened) and pairs of pages.

  python3 compare.py   (from menu_redesign/, after before/, after/ and compare/menu_*/ exist)
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'compare'
BG = (17, 20, 26)
FG = (230, 233, 239)
ACCENT = {'PŘED': (239, 120, 110), 'PO': (110, 200, 140)}


def font(size):
    for f in ('/System/Library/Fonts/Supplemental/Arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'):
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def label(img, text, color):
    head = 56
    out = Image.new('RGB', (img.width, img.height + head), BG)
    out.paste(img, (0, head))
    ImageDraw.Draw(out).text((16, 12), text, fill=color, font=font(28))
    return out


def row(images, gap=24):
    h = max(i.height for i in images)
    out = Image.new('RGB', (sum(i.width for i in images) + gap * (len(images) - 1), h), BG)
    x = 0
    for i in images:
        out.paste(i, (x, 0))
        x += i.width + gap
    return out


def column(images, gap=24):
    w = max(i.width for i in images)
    out = Image.new('RGB', (w, sum(i.height for i in images) + gap * (len(images) - 1)), BG)
    y = 0
    for i in images:
        out.paste(i, (0, y))
        y += i.height + gap
    return out


def fit(path, width):
    img = Image.open(path).convert('RGB')
    return img.resize((width, round(img.height * width / img.width)))


def menu(name):
    parts = [Image.open(p).convert('RGB') for p in sorted((OUT / name).glob('menu_*.png'))]
    return row(parts, gap=12)


def side(folder, files, tag):
    pages = column([fit(ROOT / folder / f'{f}.jpg', 900) for f in files], gap=16)
    return label(pages, f'{tag}: ' + ', '.join(files), ACCENT[tag])


def main():
    before, after = menu('menu_before'), menu('menu_after')
    column([label(before, 'PŘED: menu, postupně rozbalené sekce', ACCENT['PŘED']),
            label(after, 'PO: menu, postupně rozbalené sekce', ACCENT['PO'])], gap=40).save(OUT / 'menu.png')
    pairs = {
        'radios_roles': (['role'], ['radios_roles']),
        'radios_status': (['system_radios', 'system_maint'], ['radios_status']),
        'radios_scan': (['system_netscan'], ['radios_scan']),
        'esphome': (['ha_esphome'], ['esphome']),
        'settings_split': (['system_settings'], ['ha_matter', 'system_lan', 'system_maint']),
        'system_info': (['system_info'], ['system_info']),
        'firmware': (['system_firmware_online', 'system_firmware_settings'], ['system_firmware_online']),
        'zigbee_backups': (['zigbee_backups'], ['zigbee_backups']),
        'permissions': (['system_users_roles'], ['system_users_roles']),
    }
    for name, (b, a) in pairs.items():
        row([side('before', b, 'PŘED'), side('after', a, 'PO')], gap=40).save(OUT / f'{name}.jpg', quality=82)
    print('written:', ', '.join(sorted(p.name for p in OUT.glob('*.[jp][pn]g'))))


if __name__ == '__main__':
    main()
