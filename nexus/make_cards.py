"""The Nexus file cards (990x200, like Outfit Manager's): an emblem left, the file's kind as the title, one line under
it. Writes out/file_main_990x200.png and out/file_source_990x200.png. Built from Outfit Manager's card generator
(CDInventory/nexus/make_logo.py), with the mod's own emblem and name. make_cards.py [version]"""
import importlib.util, os, re, sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OM_LOGO = r'F:\MyProjects\CDInventory\nexus\make_logo.py'

spec = importlib.util.spec_from_file_location('om_logo', OM_LOGO)
om = importlib.util.module_from_spec(spec)
spec.loader.exec_module(om)
FONT = os.path.join(os.path.dirname(OM_LOGO), 'ref', 'Vollkorn-Medium.ttf')


def emblem(size):
    """The mouse key-guide icon inside a thin gold ring (the game's astrolabe line art)."""
    S = 4 * size
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ring = om.sprite('ring')
    la = Image.new('RGBA', ring.size, om.GOLD + (255,))
    la.putalpha(ImageChops.multiply(ring.convert('L'), ring.split()[3]).point(lambda p: int(p * 0.85)))
    im.alpha_composite(la.resize((S, S), Image.LANCZOS))
    d = ImageDraw.Draw(im)
    d.ellipse((S * 0.2, S * 0.2, S * 0.8, S * 0.8), fill=(16, 16, 20, 235), outline=om.GOLD + (200,), width=S // 90)
    ic = Image.open(os.path.join(HERE, 'sprites', 'icon_mousemove.png')).convert('RGBA')
    k = S * 0.42 / max(ic.size)
    ic = ic.resize((int(ic.width * k), int(ic.height * k)), Image.LANCZOS)
    im.alpha_composite(ic, ((S - ic.width) // 2, (S - ic.height) // 2))
    return im.resize((size, size), Image.LANCZOS)


def file_card(kind, line, sub):
    W, H = 990, 200
    bg = Image.new('RGBA', (W, H), (22, 22, 26, 255))
    bg.alpha_composite(om.tint(om.sprite('noise').resize((W, W)).crop((0, 0, W, H)), (58, 56, 54), 0.35))
    v = Image.new('L', (W, H), 0)
    ImageDraw.Draw(v).ellipse((-W // 5, -H, W + W // 5, 2 * H), fill=255)
    dark = Image.new('RGBA', (W, H), (6, 6, 8, 255))
    dark.putalpha(ImageChops.invert(v.filter(ImageFilter.GaussianBlur(40))).point(lambda p: int(p * 0.8)))
    bg.alpha_composite(dark)
    art = om.sprite('ring')
    la = Image.new('RGBA', art.size, om.GOLD + (255,))
    la.putalpha(ImageChops.multiply(art.convert('L'), art.split()[3]).point(lambda p: int(p * 0.2)))
    la = la.resize((340, 340), Image.LANCZOS)
    bg.alpha_composite(la, (W - 250, (H - la.height) // 2))
    bg.alpha_composite(emblem(168), (34, (H - 168) // 2))
    d = ImageDraw.Draw(bg)
    small, tf, sf = (ImageFont.truetype(FONT, s) for s in (17, 46, 21))
    x0 = x = 236
    for ch in 'MOUSE & WINDOW FIXES':
        d.text((x, 46), ch, font=small, fill=om.GOLD + (255,))
        x += d.textlength(ch, font=small) + 4
    d.text((x0, 66), kind, font=tf, fill=om.IVORY + (255,))
    bg.alpha_composite(om.tint(om.sprite('brush').resize((300, 7), Image.LANCZOS), om.GOLD, 0.85), (x0, 124))
    d.text((x0, 138), line, font=sf, fill=(178, 172, 164, 255))
    d.text((x0 + d.textlength(line, font=sf) + 14, 138), sub, font=sf, fill=(120, 116, 110, 255))
    return bg.convert('RGB')


if __name__ == '__main__':
    rc = open(os.path.join(os.path.dirname(HERE), 'src', 'mouse_window_fixes.rc'), encoding='utf-8').read()
    ver = sys.argv[1] if len(sys.argv) > 1 else re.search(r'#define MWF_VERSION_STR "([^"]+)"', rc).group(1)
    out = os.path.join(HERE, 'out')
    file_card('Main File', f'Version {ver}', '·  one ASI file  ·  DMM or bin64').save(
        os.path.join(out, 'file_main_990x200.png'))
    file_card('Source Code', f'Version {ver}', '·  MIT  ·  not needed to play').save(
        os.path.join(out, 'file_source_990x200.png'))
    print('ok', ver)
