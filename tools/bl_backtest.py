"""bl_backtest.py exe...: Borderless Lock's three code searches on each game build, exactly as the mod runs them
(executable sections only, the same bytes and wildcards). Prints, per build, how many places each pattern matches
(the mod needs exactly one) and where. Read-only."""
import struct, sys

# (name, pattern bytes with None = any, extra check)
RELEASE = [0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9,
           0xC6, 0x41, 0x50, 0x01, 0x33, 0xC9, 0x48, 0x8B, 0xDA, 0xFF, 0x15]
INPUT = [0x44, 0x38, 0x73, 0x30, 0x75, None, 0x44, 0x38, 0x7B, 0x32, 0x0F, 0x84, None, None, None, None, 0x80, 0x3D]
HUD_A = [0x80, 0x3D, None, None, None, None, 0x00, 0x0F, 0x84, None, None, None, None, 0x48, 0x8B, 0x59, 0x78, 0x83,
         0x7B, 0x28, 0x00]
HUD_B = [0x80, 0x7B, 0x50, 0x01, 0x0F, 0x84, None, None, None, None, 0x48, 0x8B, 0x03, 0x48, 0xC7, 0xC2, 0xFF, 0xFF,
         0xFF, 0xFF, 0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x48, 0xC6, 0x43, 0x50, 0x01]


def regex(p):
    import re
    return re.compile(b''.join(b'.' if b is None else re.escape(bytes([b])) for b in p), re.S)


def sections(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    opt = pe + 24
    sh = opt + struct.unpack_from('<H', d, pe + 20)[0]
    for i in range(n):
        o = sh + i * 40
        name = d[o:o + 8].rstrip(b'\0').decode(errors='replace')
        vs, va, rs, raw, = struct.unpack_from('<IIII', d, o + 8)
        ch = struct.unpack_from('<I', d, o + 36)[0]
        yield name, va, vs, raw, rs, ch


rel, inp, ha, hb = regex(RELEASE), regex(INPUT), regex(HUD_A), regex(HUD_B)
for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    found = {'release': [], 'input layer': [], 'HUD release': []}
    for name, va, vs, raw, rs, ch in sections(d):
        if not ch & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            continue
        blob = d[raw:raw + min(vs, rs)]
        found['release'] += [va + m.start() for m in rel.finditer(blob)]
        found['input layer'] += [va + m.start() for m in inp.finditer(blob)]
        found['HUD release'] += [va + m.start() for m in ha.finditer(blob) if hb.match(blob, m.start() + 0x41)]
    label = path.replace('\\', '/').rsplit('/', 1)[-1].replace('CrimsonDesert_', '').replace('.exe', '')
    cells = []
    for k, v in found.items():
        cells.append(f'{k}: ' + ('OK +%X' % v[0] if len(v) == 1 else f'{len(v)} matches' + (' ' + ' '.join('+%X' % x for x in v[:3]) if v else '')))
    print(f'{label:12s} ' + ' | '.join(cells))
