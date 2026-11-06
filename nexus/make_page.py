"""Nexus page art for Mouse & Window Fixes, in the same look as Outfit Manager's page: it reuses that page's
generator (CDInventory/nexus/make_nexus_art.py: the game's UI sprites and font) with this mod's text, a header of
its own and the game's mouse key-guide icons (sprites/, cut from ui/texture/cd_icon_keyguide_01.dds).
Writes out/header.png, out/sep_<section>.png, out/glance.png, out/page_mockup(_1x).png and description.bbcode.txt
(*_URL placeholders for the uploaded images). make_page.py"""
import importlib.util, os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OM_ART = r'F:\MyProjects\CDInventory\nexus\make_nexus_art.py'

spec = importlib.util.spec_from_file_location('om_art', OM_ART)
art = importlib.util.module_from_spec(spec)
spec.loader.exec_module(art)
art.OUT = os.path.join(HERE, 'out')
art.HERE = HERE                                  # write_bbcode() target folder

_om_sprite, _om_header = art.sprite, art.header


def sprite(name):
    """This mod's icons first, then Outfit Manager's sprites."""
    own = os.path.join(HERE, 'sprites', name + '.png')
    return Image.open(own).convert('RGBA') if os.path.exists(own) else _om_sprite(name)


art.sprite = sprite


def header():
    """Outfit Manager's header (plate, line art, brush, runes) with this mod's title: its text calls are muted
    while it draws."""
    S, W = art.S, art.COL * art.S
    spaced, text = art.spaced, ImageDraw.ImageDraw.text
    art.spaced, ImageDraw.ImageDraw.text = (lambda *a, **k: 0), (lambda *a, **k: None)
    try:
        im = _om_header()
    finally:
        art.spaced, ImageDraw.ImageDraw.text = spaced, text
    d = ImageDraw.Draw(im)
    cy = 128 * S
    art.spaced(d, (W / 2, cy - 42 * S), 'MOUSE & WINDOW FIXES', art.font(art.TITLE_FONT, 48 * S),
               art.INK + (255,), 7 * S)
    sub = art.font(art.TITLE_FONT, 20 * S)
    st = 'Mouse and keyboard, the way borderless should feel'
    d.text((W / 2 - d.textlength(st, font=sub) / 2, cy + 30 * S), st, font=sub, fill=art.DIM + (255,))
    return im


art.INTRO = [
    ('p', 'Five small fixes for playing in **borderless window mode with mouse and keyboard**. One file, '
          '**no game files changed**.'),
    ('glance', 'glance', [
        ('icon_move', 'No Resize', 'the window stays borderless'),
        ('icon_click', 'Game Cursor', 'no Windows arrow on clicks'),
        ('icon_mousemove', 'Camera Ready', 'no stuck menu cursor in the world'),
        ('icon_wheel', 'One-Notch Zoom', 'every wheel notch zooms'),
    ]),
    ('req', [('Requires', 'an ASI loader (DMM has one)'), ('Tested on', 'game 1.0.0.3576'),
             ('Controller', 'not affected')]),
]

art.SECTIONS = [
    ('fixes', 'What It Fixes', [
        ('step', 'No Accidental Resize', 'The borderless window has an invisible resize border, and the cursor '
                 'starts right on its corner. One click and drag there and the game **shrinks into windowed '
                 'mode**, and it\'s back to the settings. The mod turns that border off while the game covers '
                 'the screen; **windowed mode is untouched**.'),
        ('media', 'resize', ['media_resize.gif'], '', True),
        ('step', 'No Windows Arrow', 'Clicks and the screen edges no longer flash the **plain Windows arrow** '
                 'over the game.'),
        ('step', 'The Camera Works Straight Away', 'The game sometimes **leaves the mouse in menu mode** after '
                 'you are back in the world: moving the mouse **doesn\'t turn the camera**, it steers an invisible '
                 'menu cursor instead, until Kliff takes a step. It happens **when you load in** (as soon as the '
                 'HUD appears) and **when you come back to the game** from another window, with Alt+Tab or a '
                 'click. With the mod, the camera answers the mouse **right away**. Menus keep their cursor.'),
        ('step', 'A Proper Title Screen Cursor', 'No cursor over the intro. When the title prompts appear, it '
                 'starts **on the logo\'s ring** instead of the top-left corner.'),
        ('media', 'title', ['media_title.png'], '', False),
        ('step', 'One Notch, One Zoom', 'The game sometimes needed **two wheel notches** before the camera '
                 'zoomed. Now **every notch counts**.'),
    ]),
    ('installation', 'Installation', [
        ('p', '**With DMM:** add **MouseWindowFixes.asi** as an ASI mod and enable it.'),
        ('p', '**By hand:** put **MouseWindowFixes.asi** in **Crimson Desert\\bin64** (needs an ASI loader).'),
        ('p', 'To remove it, disable or delete the file. Nothing else is left behind.'),
    ]),
    ('good_to_know', 'Good to Know', [
        ('p', 'Game updates **rarely break it**: the mod finds what it needs in the game\'s code each time it '
              'starts. If one search fails after a patch, **only that fix switches off** and the rest keep '
              'working.'),
        ('p', 'After a game update, please report anything that stops working.'),
        ('spoiler', 'Compatibility, log and tested versions', [
            ('p', 'Works alongside other ASI mods, including Outfit Manager. Controllers are not affected; '
                  'only the mouse, the keyboard and the window are.'),
            ('p', 'MouseWindowFixes.log, next to the file, lists what was found at start and which fix is off '
                  'if something wasn\'t.'),
            ('p', 'Released under the MIT licence, which covers the mod\'s own work, not the game\'s content.'),
            ('p', 'Tested on Crimson Desert 1.0.0.3576 in borderless mode at 2560x1440.'),
        ]),
    ]),
    ('credits', 'Credits', [
        ('credit', 'Strahlimeier', 'idea and testing', None),
        ('credit', 'Claude (Anthropic)', 'wrote the code', None),
        ('credit', 'Tsuda Kageyu', 'MinHook, the hooking library (BSD 2-Clause licence, included)',
         'https://github.com/TsudaKageyu/minhook'),
        ('credit', 'Friedrich Althausen', 'Vollkorn, the game\'s UI font, used for these headings', None),
        ('links', [('GitHub', 'https://github.com/Yorivel/cd-mouse-window-fixes', 'source and releases'),
                   ('Source code', None, 'Files tab, under Miscellaneous')]),
    ]),
]

art.header = header


def media_image(files, caption, gif):
    """The pictures come framed and captioned already (make_media.py); a GIF shows its first frame here."""
    return Image.open(os.path.join(art.OUT, files[0])).convert('RGBA')


art.media_image = media_image
MEDIA = {b[1]: b[2][0] for _, _, blocks in art.SECTIONS for b in blocks if b[0] == 'media'}

RAW = 'https://raw.githubusercontent.com/Yorivel/cd-mouse-window-fixes/main/nexus/out/'


def write_final():
    """description.final.bbcode.txt: the placeholders swapped for the images' links in this (public) repo."""
    text = open(os.path.join(HERE, 'description.bbcode.txt'), encoding='utf-8').read()
    for key in ['header', 'glance'] + ['sep_' + k for k, _, _ in art.SECTIONS]:
        text = text.replace(key.upper() + '_URL', RAW + key + '.png')
    for key, name in MEDIA.items():
        text = text.replace('MEDIA_%s_URL' % key.upper(), RAW + name)
    assert '_URL[' not in text, 'placeholder left'
    with open(os.path.join(HERE, 'description.final.bbcode.txt'), 'w', encoding='utf-8') as f:
        f.write(text)


if __name__ == '__main__':
    art.main()
    for key, name in MEDIA.items():  # render_blocks() saves each picture as media_<key>.png: drop the GIFs' stills
        if not name.endswith('.png'):
            os.remove(os.path.join(art.OUT, 'media_%s.png' % key))
    write_final()
