"""zoom_backtest.py exe...: the one-notch zoom fix's code search on each game build, as the mod runs it (executable
sections only). The camera distance code counts wheel frames in a float (+1.0 each) and steps a zoom level only when the
count reaches a threshold (a global set at run time), draining by the frame time otherwise:

    vmovss  xmm0, [rsi+CNT]          C5 FA 10 86 <CNT>
    vaddss  xmm1, xmm0, [rip+ONE]    C5 FA 58 0D <ONE>      (the patch: [rip+THR] instead)
    vmovss  [rsi+CNT], xmm1          C5 FA 11 8E <CNT>
    vmovss  xmm0, [rsi+CNT]          C5 FA 10 86 <CNT>
    vcomiss xmm0, [rip+THR]          C5 F8 2F 05 <THR>
    jb      ...                      0F 82

Prints, per build, the matches (the mod needs exactly one), the counter offset and the constant the add reads (1.0)."""
import re, struct, sys

PAT = re.compile(rb'\xC5\xFA\x10\x86(.{4})\xC5\xFA\x58\x0D(.{4})\xC5\xFA\x11\x8E(.{4})\xC5\xFA\x10\x86(.{4})'
                 rb'\xC5\xF8\x2F\x05(.{4})\x0F\x82', re.S)


def sections(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    sh = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
    for i in range(n):
        o = sh + i * 40
        vs, va, rs, raw = struct.unpack_from('<IIII', d, o + 8)
        ch = struct.unpack_from('<I', d, o + 36)[0]
        yield va, vs, raw, rs, ch


def read_f(d, secs, r):
    for va, vs, raw, rs, ch in secs:
        if va <= r < va + rs:
            return struct.unpack_from('<f', d, r - va + raw)[0]
    return None


for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    secs = list(sections(d))
    hits = []
    for va, vs, raw, rs, ch in secs:
        if not ch & 0x20000000:
            continue
        for m in PAT.finditer(d[raw:raw + rs]):
            c = [m.group(i) for i in (1, 3, 4)]
            if c[0] != c[1] or c[0] != c[2]:
                continue
            at = va + m.start()
            add = at + 8                     # the vaddss
            one = add + 8 + struct.unpack('<i', m.group(2))[0]
            cmp_ = at + 32                   # the vcomiss
            thr = cmp_ + 8 + struct.unpack('<i', m.group(5))[0]
            hits.append((add, struct.unpack('<I', c[0])[0], one, read_f(d, secs, one), thr))
    name = path.replace('\\', '/').split('/')[-1]
    if len(hits) != 1:
        print('%-32s %d matches (need one)' % (name, len(hits)))
    for add, cnt, one, onev, thr in hits:
        print('%-32s add at +%X, counter +0x%X, adds +%X (= %s), threshold global +%X' % (name, add, cnt, one, onev, thr))
