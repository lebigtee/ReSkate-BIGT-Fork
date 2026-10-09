# Per recorded bail: which body parts touched something, in what order, and how hard (the game's
# own contact records; see probe_contacts.py for their layout).
# usage: py probe_summary.py <bail_*.bin> ...
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_analyse as pa

NAMES = {0: '?(off body)', 1: 'head', 2: 'neck', 3: 'l_hand', 4: 'l_forearm', 5: 'l_upperarm', 6: 'l_clavicle',
         7: 'r_hand', 8: 'r_forearm', 9: 'r_upperarm', 10: 'r_clavicle', 11: 'spine3', 12: 'spine2', 13: 'spine1',
         14: 'spine', 15: 'l_toes', 16: 'l_foot', 17: 'l_shin', 18: 'l_thigh', 19: 'r_toes', 20: 'r_foot',
         21: 'r_shin', 22: 'r_thigh', 23: 'pelvis'}

def summary(path):
    frames = list(pa.frames(path))
    if not frames:
        print(path, 'empty'); return
    start = next((n for n, f in enumerate(frames) if f['phase'] == 1), 0)
    t0 = frames[start]['time']
    print(f'\n### {os.path.basename(path)}: {len(frames)} frames ({frames[-1]["time"] - frames[0]["time"]:.1f} s), bail from frame {start}')
    rows = []
    for i in range(24):
        first, peak, frames_touching = None, 0.0, 0
        for f in frames:
            r = f['contacts'][0x2d0 + i * 0x80:0x2d0 + (i + 1) * 0x80]
            if len(r) < 0x80 or not r[0x73]: continue
            frames_touching += 1
            hit = max(v for v in (struct.unpack_from('<f', r, o)[0] for o in (0x30, 0x34, 0x60, 0x64)) if v == v and v < 30)
            if first is None: first = f['time'] - t0
            peak = max(peak, hit)
        if frames_touching: rows.append((first, i, frames_touching, peak))
    for first, i, n, peak in sorted(rows):
        print(f'  {first:+6.2f}s  part {i:2d} {NAMES[i]:12s} peak {peak:5.2f} m/s  ({n} frames touching)')

if __name__ == '__main__':
    for p in sys.argv[1:]: summary(p)
