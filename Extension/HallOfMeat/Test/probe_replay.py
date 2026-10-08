# Replays recorded bails through the Hall of Meat's rules with the game's contacts (as
# hall_of_meat.cpp reads them): which bones each bail would break. Keep the tables in step with
# hall_of_meat.h (fragility) and hall_of_meat.cpp (thresholds, point_bone, contact_reach).
# usage: py probe_replay.py <bail_*.bin> ...
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_analyse as pa

NAMES = ['Skull', 'Neck', 'Right collarbone', 'Left collarbone', 'Ribs', 'Spine', 'Pelvis', 'Right humerus', 'Left humerus',
         'Right forearm', 'Left forearm', 'Right wrist', 'Left wrist', 'Right femur', 'Left femur', 'Right tibia', 'Left tibia',
         'Right ankle', 'Left ankle']
FRAGILITY = [1.15, 0.9, 1.0, 1.0, 1.0, 0.85, 0.9, 0.9, 0.9, 0.95, 0.95, 1.0, 1.0, 0.75, 0.75, 0.9, 0.9, 0.95, 0.95]
THRESHOLDS = [(9.0, 'Cracked'), (11.0, 'Fractured'), (13.5, 'Compound fracture'), (16.0, 'Shattered')]
POINT_BONE = [6, 5, 5, 4, 4, 1, 1, 0, 0, 2, 7, 9, 11, 11, 3, 8, 10, 12, 12, 13, 15, 17, 17, 14, 16, 18, 18]
SKULL_POINT, REACH = 8, 0.12

def contact_bones(f):
    bones = [0.0] * 19
    for i in range(24):
        r = f['contacts'][0x2d0 + i * 0x80:0x2d0 + (i + 1) * 0x80]
        if len(r) < 0x80: continue
        at = struct.unpack_from('<3f', r, 0)
        best, nearest = REACH, None
        for p, point in enumerate(f['points']):
            if p == SKULL_POINT: continue
            d = pa.distance(at, point)
            if d < best: best, nearest = d, p
        if nearest is None or not r[0x73]: continue
        hit = max([v for v in (struct.unpack_from('<f', r, o)[0] for o in (0x30, 0x34, 0x60, 0x64)) if v == v and 0 <= v < 30] or [0])
        b = POINT_BONE[nearest]
        bones[b] = max(bones[b], hit)
    return bones

def replay(path):
    frames = list(pa.frames(path))
    start = next((n for n, f in enumerate(frames) if f['phase'] == 1), None)
    if start is None: print(os.path.basename(path), ': no bail'); return
    t0 = frames[start]['time']
    peaks = [0.0] * 19
    for f in frames:
        if f['phase'] == 1 or t0 - 0.4 <= f['time'] < t0:
            peaks = [max(a, b) for a, b in zip(peaks, contact_bones(f))]
    result = []
    for k in range(19):
        e = peaks[k] * FRAGILITY[k]
        level = None
        for threshold, name in THRESHOLDS:
            if e >= threshold: level = name
        if level: result.append((e, f'{NAMES[k]} ({level})'))
    result.sort(reverse=True)
    print(f'{os.path.basename(path)}: {len(result)} broken' + (': ' + ', '.join(r for _, r in result) if result else ''))

if __name__ == '__main__':
    for p in sys.argv[1:]: replay(p)
