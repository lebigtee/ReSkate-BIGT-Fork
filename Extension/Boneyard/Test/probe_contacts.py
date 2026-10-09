# The 24 per-part records as the rig publisher (0x4769d20) copies them into the animation's contact
# block ([[core+0x3b8]+0x30]): record i at +0x2d0 + i*0x80. Per record (offsets in the copy, and
# where the copy reads them in the collision object's record of 0x70 bytes):
#   +0x00 vec  per-part vector [object+0x10c0][i+0x106]   +0x40 vec  rec+0x10
#   +0x10 vec  rec+0x00                                     +0x50 vec  rec+0x30
#   +0x20 vec  rec+0x20                                     +0x60 f    |dot(+0x00, rec+0x10)|
#   +0x30 f    |dot(+0x00, rec+0x00)|                       +0x64 f    rec+0x54, +0x68 f rec+0x5c
#   +0x34 f    rec+0x50, +0x38 f rec+0x58                   +0x70..+0x74 bytes rec+0x60..+0x64
# usage: py probe_contacts.py <bail_*.bin> ...
import struct, sys, math, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
import probe_analyse as pa

BASE, STRIDE, PARTS = 0x2d0, 0x80, 24
VECS = {0x00: 'v', 0x10: 'a', 0x20: 'b', 0x40: 'c', 0x50: 'd'}
FLOATS = {0x30: 'dot0', 0x34: 'f34', 0x38: 'f38', 0x60: 'dot1', 0x64: 'f64', 0x68: 'f68'}

def part(contacts, i):
    at = BASE + i * STRIDE
    return contacts[at:at + STRIDE] if len(contacts) >= at + STRIDE else None

def main(paths):
    frames = [f for p in paths for f in pa.frames(p)]
    print(f'{len(frames)} frames')
    print('\n== vec fields that track a skeleton point (mean distance, m; how often finite) ==')
    for i in range(PARTS):
        out = []
        for off, name in VECS.items():
            tally = collections.defaultdict(list); finite = 0
            for f in frames:
                r = part(f['contacts'], i)
                if not r: continue
                v = struct.unpack_from('<3f', r, off)
                if not all(math.isfinite(x) and abs(x) < 1e5 for x in v) or max(abs(x) for x in v) < 1e-6: continue
                finite += 1
                for k, p in enumerate(f['points']): tally[k].append(pa.distance(v, p))
            if tally:
                k, ds = min(tally.items(), key=lambda kv: sum(kv[1]) / len(kv[1]))
                out.append(f'{name}~{pa.POINTS[k]} {sum(ds) / len(ds):.2f}m ({finite})')
        print(f'part {i:2d}: ' + '; '.join(out))
    print('\n== floats and flags over the bail (changes, first 10 per part) ==')
    t0 = frames[0]['time']
    for i in range(PARTS):
        last, rows = None, []
        for f in frames:
            r = part(f['contacts'], i)
            if not r: continue
            floats = tuple(round(struct.unpack_from('<f', r, o)[0], 2) for o in FLOATS)
            flags = tuple(r[0x70:0x75])
            key = (floats, flags)
            if key != last:
                top = max(range(19), key=lambda b: f['hits'][b])
                rows.append(f"t={f['time'] - t0:5.2f} ph={f['phase']} {dict(zip(FLOATS.values(), floats))} flags={flags} | hardest {pa.BONES[top]} {f['hits'][top]:.1f}")
                last = key
        if len(rows) > 1:
            print(f'part {i:2d}: {len(rows)} changes')
            for row in rows[:10]: print('   ', row)

if __name__ == '__main__':
    main(sys.argv[1:])
