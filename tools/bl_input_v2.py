"""bl_input_v2.py exe...: the input-layer search, version 2 (offsets read from the code), on each build.
Anchor: mov ecx,0x2B; bt ecx,r32 (B9 2B 00 00 00 44 0F A3 ..) - exactly one. Before it (within 0x80 bytes): the
layer compare cmp byte [rbx+disp8], r8b (44 38 ModRM disp8, ModRM mod=01 rm=011) followed by jne short (75 ..) - the
last such before the anchor whose next compare is the sub-layer one; the mouse-handling global cmp byte [rip+x],0
(80 3D rel32 00) between them. Function start: the exception table (.pdata via the exception directory)."""
import bisect, re, struct, sys

ANCHOR = re.compile(rb'\xB9\x2B\x00\x00\x00\x44\x0F\xA3', re.S)
CMP = re.compile(rb'\x44\x38([\x43\x4B\x53\x5B\x63\x6B\x73\x7B])(.)', re.S)  # cmp [rbx+d8], r8b..r15b
GLOBAL = re.compile(rb'\x80\x3D(....)\x00', re.S)


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    sh = opt + struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(n):
        o = sh + i * 40
        vs, va, rs, raw = struct.unpack_from('<IIII', d, o + 8)
        secs.append((d[o:o + 8].rstrip(b'\0').decode(errors='replace'), va, vs, raw, rs, struct.unpack_from('<I', d, o + 36)[0]))
    exc_va, exc_size = struct.unpack_from('<II', d, opt + 112 + 3 * 8)
    return d, secs, exc_va, exc_size


def off(secs, r):
    for nm, va, vs, raw, rs, ch in secs:
        if va <= r < va + rs:
            return raw + r - va


for path in sys.argv[1:]:
    d, secs, exc_va, exc_size = load(path)
    label = path.replace('\\', '/').rsplit('/', 1)[-1].replace('CrimsonDesert_', '').replace('.exe', '')
    anchors = []
    for nm, va, vs, raw, rs, ch in secs:
        if ch & 0x20000000:
            blob = d[raw:raw + min(vs, rs)]
            anchors += [(va + m.start(), raw + m.start()) for m in ANCHOR.finditer(blob)]
    if len(anchors) != 1:
        print(f'{label:12s} anchor: {len(anchors)} matches')
        continue
    a_rva, a_fo = anchors[0]
    win = d[a_fo - 0x80:a_fo]
    cmps = [(m.start(), m.group(2)[0]) for m in CMP.finditer(win)]
    # the layer compare: the first of two compares 2 bytes apart in offset (layer, then sub-layer at +2)
    layer = None
    for (p1, d1), (p2, d2) in zip(cmps, cmps[1:]):
        if d2 == d1 + 2 and p2 - p1 == 6 and win[p1 + 4] == 0x75:  # (cmp; jne short; cmp sub-layer)
            layer = d1
            glob_m = GLOBAL.search(win, p2)
            break
    g = None
    if layer is not None and glob_m:
        g_fo = a_fo - 0x80 + glob_m.start()
        g = a_rva - 0x80 + glob_m.start() + 7 + struct.unpack_from('<i', glob_m.group(1))[0]
    # function start from .pdata
    starts = []
    po = off(secs, exc_va)
    for k in range(exc_size // 12):
        b, e, u = struct.unpack_from('<III', d, po + k * 12)
        if b <= a_rva < e:
            starts.append(b)
    print(f'{label:12s} anchor +{a_rva:X}; layer offset ' + (f'+0x{layer:X}' if layer is not None else 'NOT FOUND') +
          (f'; mouse global +{g:X}' if g else '; global NOT FOUND') + (f'; function +{starts[0]:X}' if starts else '; no .pdata entry'))
