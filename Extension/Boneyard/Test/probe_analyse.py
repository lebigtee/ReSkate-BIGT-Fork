# Reads the Boneyard's probe recordings (`boneyard probe on`, boneyard.cpp) and looks for the
# game's own per-body-part contact records in them:
#   - which vec4 fields of the 24 records (and of the per-part vectors) are world positions, and of
#     which skeleton point (so each record is a body part);
#   - which flags and floats of each record change when that part is hit.
# usage: py probe_analyse.py <bail_*.bin> [more files]
import struct, sys, math, collections

POINTS = ['hips', 'spine', 'spine1', 'spine2', 'spine3', 'neck', 'neck1', 'head', 'skull',
          'r_clavicle', 'r_shoulder', 'r_elbow', 'r_wrist', 'r_knuckles',
          'l_clavicle', 'l_shoulder', 'l_elbow', 'l_wrist', 'l_knuckles',
          'r_hip', 'r_knee', 'r_ankle', 'r_toes', 'l_hip', 'l_knee', 'l_ankle', 'l_toes']
BONES = ['skull', 'neck', 'r_collarbone', 'l_collarbone', 'ribs', 'spine', 'pelvis', 'r_humerus', 'l_humerus',
         'r_forearm', 'l_forearm', 'r_wrist', 'l_wrist', 'r_femur', 'l_femur', 'r_tibia', 'l_tibia', 'r_ankle', 'l_ankle']
OBJECT_FROM = 0x380
RECORDS, RECORD_SIZE, RECORD_AT = 24, 0x70, 0x5e0

def frames(path):
    data = open(path, 'rb').read()
    at = 0
    while at + 8 <= len(data):
        try:
            frame = _frame(data, at, path)
        except struct.error:
            return  # the last frame is still being written
        if frame is None: return
        at, value = frame
        yield value

def _frame(data, at, path):
    if True:
        magic, version = struct.unpack_from('<II', data, at)
        if magic != 0x504d4f48 or version != 1:
            raise ValueError(f'{path}: bad frame at {at}')
        at += 8
        time, phase, state, serial = struct.unpack_from('<dIII', data, at); at += 20
        points = [struct.unpack_from('<3f', data, at + i * 12) for i in range(27)]; at += 27 * 12
        hits = struct.unpack_from('<19f', data, at); at += 19 * 4
        hips_hit, hips_speed = struct.unpack_from('<2f', data, at); at += 8
        core, obj, contacts = struct.unpack_from('<3Q', data, at); at += 24
        blocks = []
        for _ in range(3):
            size = struct.unpack_from('<I', data, at)[0]; at += 4
            blocks.append(data[at:at + size]); at += size
        return at, dict(time=time, phase=phase, state=state, serial=serial, points=points, hits=hits,
                   hips_hit=hips_hit, hips_speed=hips_speed, object=blocks[0], contacts=blocks[1], vectors=blocks[2])

def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))

def record(obj, i):
    at = RECORD_AT - OBJECT_FROM + i * RECORD_SIZE
    return obj[at:at + RECORD_SIZE] if len(obj) >= at + RECORD_SIZE else None

def analyse(paths):
    all_frames = [f for p in paths for f in frames(p)]
    print(f'{len(all_frames)} frames from {len(paths)} file(s)')
    if not all_frames or not all_frames[0]['object']:
        print('no collision object recorded'); return
    # 1. Positions: for each record and each vec4 field, the skeleton point it stays closest to.
    print('\n== record fields that track a skeleton point (mean distance, metres) ==')
    for i in range(RECORDS):
        best = []
        for field in range(0, RECORD_SIZE - 12, 16):
            tally = collections.defaultdict(list)
            for f in all_frames:
                r = record(f['object'], i)
                if not r: continue
                v = struct.unpack_from('<3f', r, field)
                if not all(math.isfinite(x) and abs(x) < 1e5 for x in v): continue
                for k, p in enumerate(f['points']):
                    tally[k].append(distance(v, p))
            if tally:
                k, ds = min(tally.items(), key=lambda kv: sum(kv[1]) / len(kv[1]))
                mean = sum(ds) / len(ds)
                if mean < 0.35: best.append(f'+{field:#04x} ~ {POINTS[k]} ({mean:.2f} m)')
        print(f'record {i:2d}: ' + ('; '.join(best) if best else '-'))
    print('\n== per-part vectors that track a skeleton point ==')
    for v in range(0x50):
        tally = collections.defaultdict(list)
        for f in all_frames:
            if len(f['vectors']) < (v + 1) * 16: continue
            x = struct.unpack_from('<3f', f['vectors'], v * 16)
            if not all(math.isfinite(c) and abs(c) < 1e5 for c in x): continue
            for k, p in enumerate(f['points']): tally[k].append(distance(x, p))
        if tally:
            k, ds = min(tally.items(), key=lambda kv: sum(kv[1]) / len(kv[1]))
            mean = sum(ds) / len(ds)
            if mean < 0.35: print(f'vector {v + 0xd2:#x}: ~ {POINTS[k]} ({mean:.2f} m)')
    # 2. Flags and floats: which change during the bail, and when compared with the measured hits.
    print('\n== record bytes +0x60..+0x64 and floats +0x50..+0x5c over time (changes only) ==')
    for i in range(RECORDS):
        last = None
        changes = []
        for f in all_frames:
            r = record(f['object'], i)
            if not r: continue
            flags = tuple(r[0x60:0x65])
            floats = struct.unpack_from('<4f', r, 0x50)
            key = (flags, tuple(round(x, 2) for x in floats))
            if key != last:
                top = max(range(19), key=lambda b: f['hits'][b])
                changes.append(f"t={f['time'] - all_frames[0]['time']:.2f} phase={f['phase']} flags={flags} f={tuple(round(x, 2) for x in floats)} hardest={BONES[top]} {f['hits'][top]:.1f}")
                last = key
        if len(changes) > 1:
            print(f'record {i:2d}: {len(changes)} changes'); [print('   ', c) for c in changes[:12]]

if __name__ == '__main__':
    analyse(sys.argv[1:])
