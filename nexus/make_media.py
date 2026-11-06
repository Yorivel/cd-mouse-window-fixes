"""Pictures for the Nexus page and the thumbnail, cut from the user's recordings (2560x1440):
  out/media_resize.gif  the vanilla bug: a drag on the corner shrinks the game out of borderless. Everything outside
                        the game window (the desktop) is pixelated and blurred past reading, frame by frame.
  out/media_title.png   title screen cursor: vanilla (top-left corner) beside the mod's (on the logo's ring)
  out/thumbnail.png     1920x1080 main image (and thumbnail_1280.jpg)
Raw frames go to a temp folder outside the repo (they show the desktop unblurred) and are deleted afterwards.
make_media.py"""
import glob, os, shutil, subprocess, tempfile
from PIL import Image, ImageDraw, ImageFilter

import make_page                                  # Outfit Manager's art module, set up for this mod
art = make_page.art

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
FFMPEG = shutil.which('ffmpeg') or os.path.expandvars(r'%LOCALAPPDATA%\Microsoft\WinGet\Links\ffmpeg.exe')
VIDS = os.path.expanduser(r'~\Videos\NVIDIA\Crimson Desert')                     # the recordings
RESIZE_VIDEO = os.path.join(VIDS, 'Crimson Desert 2026.11.06 - 14.51.49.06.mp4')    # vanilla, title screen
WORLD_VIDEO = os.path.join(VIDS, 'Crimson Desert 2026.11.06 - 14.40.43.05.mp4')     # village, Kliff in armour
MOD_TITLE = os.path.join(HERE, 'media_src', 'title_with_mod.png')                   # with the mod, cursor drawn in
RESIZE_SPAN = (40.6, 45.8)
GIF_W, FPS = 720, 12


def ff(*args):
    subprocess.run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-y'] + list(args), check=True)


def frame_at(video, t, path):
    ff('-ss', str(t), '-i', video, '-frames:v', '1', path)
    return Image.open(path).convert('RGB')


def game_rect(im):
    """The game window inside a desktop frame. The window is pinned bottom-right while its top-left corner is
    dragged; the game's title screen is pure black there, the desktop around it is not. Returns (x0, y0)."""
    g = im.convert('L')
    W, H = g.size
    px = g.load()

    def black_share(xs, ys):
        n = sum(1 for x in xs for y in ys if px[x, y] <= 6)
        return n / (len(xs) * len(ys))

    x0 = next((x for x in range(0, W // 2) if black_share([x], range(H - 520, H - 200, 4)) > 0.97), 0)
    y0 = next((y for y in range(0, H // 2) if black_share(range(W - 900, W - 300, 4), [y]) > 0.97), 0)
    return x0, y0


def hide_desktop(im, rect):
    """Pixelate, then blur, everything outside the game window, and darken it a little."""
    x0, y0 = rect
    if x0 == 0 and y0 == 0:
        return im
    W, H = im.size
    k = 40
    mush = im.resize((W // k, H // k), Image.BILINEAR).resize((W, H), Image.NEAREST)
    mush = mush.filter(ImageFilter.GaussianBlur(28)).point(lambda v: int(v * 0.8))
    out = mush.copy()
    out.paste(im.crop((x0, y0, W, H)), (x0, y0))
    return out


def resize_gif():
    tmp = tempfile.mkdtemp(prefix='mwf_')
    try:
        a, b = RESIZE_SPAN
        ff('-ss', str(a), '-t', str(b - a), '-i', RESIZE_VIDEO, '-vf', 'fps=%d' % FPS, os.path.join(tmp, 'f%04d.png'))
        frames = sorted(glob.glob(os.path.join(tmp, 'f*.png')))
        rects = []
        for f in frames:
            im = Image.open(f).convert('RGB')
            r = game_rect(im)
            rects.append(r)
            im = hide_desktop(im, r).resize((GIF_W, GIF_W * 9 // 16), Image.LANCZOS)
            im.save(f)
        print('game window corners', rects)
        w, h = GIF_W, GIF_W * 9 // 16
        cap = 'Without the mod: one drag on the corner and the game falls out of borderless'
        ov, pad = frame_overlay(w, h, cap)
        ovp = os.path.join(tmp, 'frame.png')
        ov.save(ovp)
        # hold the last frame a moment before the loop restarts
        out = os.path.join(OUT, 'media_resize.gif')
        graph = ('[0:v]tpad=stop_mode=clone:stop_duration=1.5,pad=%d:%d:%d:%d:color=0x29292e[v];[v][1:v]overlay=0:0,'
                 'split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];'
                 '[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle' % (ov.width, ov.height, pad, pad))
        ff('-framerate', str(FPS), '-i', os.path.join(tmp, 'f%04d.png'), '-i', ovp, '-filter_complex', graph,
           '-loop', '0', out)
        print(out, '%.1f MB' % (os.path.getsize(out) / 1e6))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def frame_overlay(w, h, caption):
    """The page's picture frame around a w x h picture: thin gold rule, corner diamonds, caption below."""
    pad = 10
    W, H = w + 2 * pad, h + 2 * pad + 40
    im = Image.new('RGBA', (W, H), art.BG + (255,))
    d = ImageDraw.Draw(im)
    d.rectangle((pad, pad, pad + w - 1, pad + h - 1), fill=(0, 0, 0, 0))
    x0, y0, x1, y1 = pad - 1, pad - 1, pad + w, pad + h
    d.rectangle((x0, y0, x1, y1), outline=art.GOLD + (120,), width=1)
    for cx, cy in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
        d.polygon([(cx, cy - 5), (cx + 5, cy), (cx, cy + 5), (cx - 5, cy)], fill=art.GOLD + (230,))
    f = art.font(art.TITLE_FONT, 17)
    d.text((W / 2 - d.textlength(caption, font=f) / 2, pad + h + 12), caption, font=f, fill=art.DIM + (255,))
    return im, pad


def labelled(im, label):
    """A small tag in the picture's bottom-left corner, in the game's font."""
    d = ImageDraw.Draw(im)
    f = art.font(art.TITLE_FONT, 16)
    tw = d.textlength(label, font=f)
    y = im.height - 38
    d.rounded_rectangle((10, y, 10 + tw + 18, y + 28), 5, fill=(20, 20, 24, 225), outline=art.GOLD + (200,))
    d.text((19, y + 3), label, font=f, fill=art.GOLD + (255,))
    return im


def title_pair():
    """Vanilla: the cursor sits in the top-left corner. With the mod: on the logo's ring. Each crop is 16:9 at
    the same scale, so the two read side by side; the vanilla crop is the corner, the mod's the logo."""
    tmp = tempfile.mkdtemp(prefix='mwf_')
    try:
        van = frame_at(RESIZE_VIDEO, 41.5, os.path.join(tmp, 'v.png'))   # still full screen, before the drag
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    mod = Image.open(MOD_TITLE).convert('RGB')
    a = van.crop((0, 0, 960, 540))                      # same scale as the mod side
    cx, cy, cw, ch = 1341, 714, 960, 540                # closer in on the ring (cursor spot at 2560x1440)
    b = mod.crop((cx - cw // 2, cy - ch // 2, cx + cw // 2, cy + ch // 2))
    w = 430
    a = labelled(a.resize((w, w * 9 // 16), Image.LANCZOS).convert('RGBA'), 'Vanilla')
    b = labelled(b.resize((w, w * 9 // 16), Image.LANCZOS).convert('RGBA'), 'With the mod')
    # the vanilla arrow is tiny at this size: a gold ring around it
    d = ImageDraw.Draw(a)
    d.ellipse((-20, -20, 40, 40), outline=art.GOLD + (230,), width=2)
    gap, pad = 16, 10
    h = a.height
    W = 2 * w + gap + 2 * pad
    im = Image.new('RGBA', (W, h + 2 * pad + 40), art.BG + (255,))
    dd = ImageDraw.Draw(im)
    for i, s in enumerate((a, b)):
        x = pad + i * (w + gap)
        im.alpha_composite(s, (x, pad))
        x0, y0, x1, y1 = x - 1, pad - 1, x + w, pad + h
        dd.rectangle((x0, y0, x1, y1), outline=art.GOLD + (120,), width=1)
        for qx, qy in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
            dd.polygon([(qx, qy - 5), (qx + 5, qy), (qx, qy + 5), (qx - 5, qy)], fill=art.GOLD + (230,))
    cap = 'The title screen cursor: the top-left corner, or the logo\'s ring'
    f = art.font(art.TITLE_FONT, 17)
    dd.text((W / 2 - dd.textlength(cap, font=f) / 2, pad + h + 12), cap, font=f, fill=art.DIM + (255,))
    out = os.path.join(OUT, 'media_title.png')
    im.convert('RGB').save(out)
    print(out)


def thumbnail():
    """1920x1080: the village behind a dark veil, the title in the game's font, the four fixes' icons below."""
    tmp = tempfile.mkdtemp(prefix='mwf_')
    try:
        bg = frame_at(WORLD_VIDEO, 116, os.path.join(tmp, 'w.png'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # drop the top strip (another mod's coordinates overlay) and the HUD's quest/minimap corner
    bg = bg.crop((640, 160, 2560, 160 + 1080)).resize((1920, 1080), Image.LANCZOS).convert('RGBA')
    bg = bg.filter(ImageFilter.GaussianBlur(3))
    veil = Image.new('RGBA', bg.size, (0, 0, 0, 0))
    vd = ImageDraw.Draw(veil)
    for y in range(1080):                                      # darker in the middle band where the text sits
        t = 1 - abs(y - 500) / 620
        vd.line((0, y, 1920, y), fill=(14, 14, 18, int(120 + 110 * max(0, t))))
    bg.alpha_composite(veil)
    d = ImageDraw.Draw(bg)
    W = 1920
    art.spaced(d, (W / 2, 330), 'MOUSE & WINDOW', art.font(art.TITLE_FONT, 120), art.INK + (255,), 16)
    art.spaced(d, (W / 2, 470), 'FIXES', art.font(art.TITLE_FONT, 120), art.INK + (255,), 16)
    brush = art.tint(art.sprite('brush').resize((760, 16)), art.GOLD, 0.9)
    bg.alpha_composite(brush, (W // 2 - brush.width // 2, 620))
    sub = art.font(art.TITLE_FONT, 38)
    st = 'for borderless mode, mouse and keyboard'
    d.text((W / 2 - d.textlength(st, font=sub) / 2, 655), st, font=sub, fill=(200, 194, 186, 255))
    icons = ['icon_move', 'icon_click', 'icon_mousemove', 'icon_wheel']
    size, gap = 96, 70
    x = W / 2 - (len(icons) * size + (len(icons) - 1) * gap) / 2
    for name in icons:
        ic = art.sprite(name)
        k = size / max(ic.size)
        ic = ic.resize((int(ic.width * k), int(ic.height * k)), Image.LANCZOS)
        bg.alpha_composite(ic, (int(x + (size - ic.width) / 2), int(800 + (size - ic.height) / 2)))
        x += size + gap
    out = os.path.join(OUT, 'thumbnail.png')
    bg.convert('RGB').save(out)
    bg.convert('RGB').resize((1280, 720), Image.LANCZOS).save(os.path.join(OUT, 'thumbnail_1280.jpg'), quality=92)
    print(out)


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    resize_gif()
    title_pair()
    thumbnail()
