# The Boneyard's skeleton drawings, measured: for each view (assets/skeleton front|back|side,
# 1024 x 1536 as drawn), each bone's two joints and the box its art fills, in the drawing's own
# pixels. Prints the sprite tables for boneyard_hud.cpp (`cpp`) and the cut-out quads to
# check them over the drawings (`quads`, read by BoneyardAssets.Overlay).
# usage: py skeleton_spec.py cpp|quads|mask (the profile's arm, as four corners a line)
import math, sys

# name, from point, to point, bone (severity), a, b, art: ('box', x0, y0, x1, y1) or
# ('band', before, after, half width[, right half width]), breadth ('round' or 'torso'), twin (True:
# the other side's bone too, mirrored in front and back, the same art in profile)
FRONT = [  # image left = the skater's right
    ('pelvis',    'right_hip', 'left_hip', 6, (420, 772), (604, 772), ('box', 388, 638, 636, 840), 'round', False),
    ('lumbar',    'spine1', 'spine', 5, (512, 530), (512, 655), ('box', 474, 528, 550, 670), 'round', False),
    ('ribcage',   'spine3', 'spine1', 4, (512, 290), (512, 515), ('box', 380, 258, 644, 570), 'torso', False),
    ('neck',      'neck', 'head', 1, (512, 262), (512, 202), ('box', 484, 198, 540, 268), 'round', False),
    ('skull',     'head', 'skull', 0, (512, 200), (512, 116), ('box', 442, 8, 584, 206), 'torso', False),
    ('femur',     'right_hip', 'right_knee', 13, (420, 772), (426, 1042), ('band', 22, 4, 42, 32), 'round', True),
    ('tibia',     'right_knee', 'right_ankle', 15, (425, 1052), (428, 1384), ('box', 378, 1046, 458, 1390), 'round', True),
    ('foot',      'right_ankle', 'right_toes', 17, (428, 1392), (422, 1465), ('box', 374, 1386, 466, 1530), 'round', True),
    ('clavicle',  'right_clavicle', 'right_shoulder', 2, (468, 296), (348, 293), ('box', 332, 281, 472, 307), 'round', True),
    ('humerus',   'right_shoulder', 'right_elbow', 7, (340, 322), (272, 558), ('band', 20, 10, 30), 'round', True),
    ('forearm',   'right_elbow', 'right_wrist', 9, (270, 572), (212, 740), ('band', 6, 2, 30, 22), 'round', True),
    ('hand',      'right_wrist', 'right_knuckles', 11, (197, 760), (178, 812), ('band', 6, 102, 68, 64), 'round', True),
]
BACK = [  # image left = the skater's left
    ('pelvis',    'right_hip', 'left_hip', 6, (606, 758), (418, 758), ('box', 383, 614, 641, 820), 'round', False),
    ('lumbar',    'spine1', 'spine', 5, (512, 525), (512, 652), ('box', 474, 503, 550, 670), 'round', False),
    ('ribcage',   'spine3', 'spine1', 4, (512, 285), (512, 512), ('box', 382, 246, 644, 565), 'torso', False),
    ('neck',      'neck', 'head', 1, (512, 262), (512, 178), ('box', 484, 175, 542, 266), 'round', False),
    ('skull',     'head', 'skull', 0, (512, 175), (512, 91), ('box', 442, 24, 582, 180), 'torso', False),
    ('femur',     'left_hip', 'left_knee', 14, (418, 758), (432, 1045), ('band', 22, 5, 44, 34), 'round', True),
    ('tibia',     'left_knee', 'left_ankle', 16, (434, 1052), (446, 1414), ('box', 385, 1046, 476, 1420), 'round', True),
    ('foot',      'left_ankle', 'left_toes', 18, (446, 1422), (442, 1478), ('box', 366, 1418, 472, 1528), 'round', True),
    ('clavicle',  'left_clavicle', 'left_shoulder', 3, (470, 296), (348, 292), ('box', 330, 278, 476, 306), 'round', True),
    ('humerus',   'left_shoulder', 'left_elbow', 8, (338, 322), (288, 560), ('band', 20, 10, 30), 'round', True),
    ('forearm',   'left_elbow', 'left_wrist', 10, (285, 580), (224, 746), ('band', 6, 2, 30, 22), 'round', True),
    ('hand',      'left_wrist', 'left_knuckles', 12, (215, 768), (195, 825), ('band', 6, 104, 70, 50), 'round', True),
]
SIDE = [  # a profile facing right; each limb's art serves both sides
    ('pelvis',    'spine', 'right_hip', 6, (470, 662), (505, 792), ('box', 432, 650, 578, 852), 'round', False),
    ('lumbar',    'spine1', 'spine', 5, (492, 560), (498, 640), ('box', 462, 556, 524, 650), 'round', False),
    ('ribcage',   'spine3', 'spine1', 4, (482, 330), (492, 555), ('box', 412, 298, 608, 592), 'round', False),
    ('neck',      'neck', 'head', 1, (472, 305), (508, 220), ('band', 6, 4, 26), 'round', False),
    ('skull',     'head', 'skull', 0, (510, 218), (510, 134), ('box', 442, 84, 602, 272), 'round', False),
    ('femur',     'right_hip', 'right_knee', 13, (505, 792), (508, 1098), ('box', 470, 780, 545, 1104), 'round', True),
    ('tibia',     'right_knee', 'right_ankle', 15, (500, 1108), (490, 1430), ('box', 455, 1100, 545, 1438), 'round', True),
    ('foot',      'right_ankle', 'right_toes', 17, (488, 1440), (560, 1462), ('box', 442, 1426, 622, 1486), 'round', True),
    ('clavicle',  'right_clavicle', 'right_shoulder', 2, (520, 322), (470, 318), ('box', 436, 306, 536, 340), 'round', True),
    ('humerus',   'right_shoulder', 'right_elbow', 7, (480, 330), (500, 615), ('band', 20, 10, 28), 'round', True),
    ('forearm',   'right_elbow', 'right_wrist', 9, (500, 625), (546, 800), ('band', 6, 0, 26), 'round', True),
    ('hand',      'right_wrist', 'right_knuckles', 11, (552, 812), (565, 872), ('band', 6, 100, 50, 64), 'round', True),
]
VIEWS = [('front', FRONT), ('back', BACK), ('side', SIDE)]
# The profile's arm lies over its ribs, pelvis and thigh: the arm takes its art from a copy with
# only the arm in it (these quads, a little wider than the arm's own), the rest from a copy with
# the arm cleared, so neither drags pieces of the other along.
SIDE_ARM = ('humerus', 'forearm', 'hand')
ARM_MASK = [((480, 330), (500, 615), (14, 4, 22, 22)), ((500, 625), (548, 805), (8, 4, 22, 22)),
            ((552, 812), (565, 872), (14, 100, 30, 66))]


def extents(a, b, art):
    """before, after, left, right as boneyard_hud.cpp's Sprite has them."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dy)
    d = (dx / length, dy / length)
    n = (-d[1], d[0])
    if art[0] == 'band':
        return art[1], art[2], art[3], art[4] if len(art) > 4 else art[3]
    _, x0, y0, x1, y1 = art
    us, vs = [], []
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        rx, ry = x - a[0], y - a[1]
        us.append(rx * d[0] + ry * d[1])
        vs.append(rx * n[0] + ry * n[1])
    return max(0, -min(us)), max(0, max(us) - length), max(0, max(vs)), max(0, -min(vs))


def quad(a, b, e):
    before, after, left, right = e
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dy)
    d = (dx / length, dy / length)
    n = (-d[1], d[0])
    return [(a[0] + d[0] * u + n[0] * v, a[1] + d[1] * u + n[1] * v)
            for u, v in ((-before, left), (length + after, left), (length + after, -right), (-before, -right))]


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'cpp'
    for view, sprites in VIEWS:
        if what == 'cpp':
            print(f'constexpr std::array<Sprite, {len(sprites)}> {view}_sprites{{{{')
        for name, p, q, bone, a, b, art, breadth, twin in sprites:
            e = extents(a, b, art)
            # The drawings in the sheet: front, back, the profile's arm, and the profile without it.
            picture = {'front': 0, 'back': 1}.get(view, 2 if name in SIDE_ARM else 3)
            if what == 'cpp':
                print(f'    {{yard::{p}, yard::{q}, {bone}, {picture}, {{{a[0]}, {a[1]}}}, {{{b[0]}, {b[1]}}}, '
                      f'{e[0]:.0f}, {e[1]:.0f}, {e[2]:.0f}, {e[3]:.0f}, Breadth::{breadth}, {"true" if twin else "false"}}}, // {name}')
            elif what == 'quads':
                pts = quad(a, b, e)
                print(view, name, ' '.join(f'{x:.1f} {y:.1f}' for x, y in pts), a[0], a[1], b[0], b[1])
        if what == 'cpp':
            print('}};')
    if what == 'mask':
        for a, b, e in ARM_MASK:
            print(' '.join(f'{x:.1f} {y:.1f}' for x, y in quad(a, b, e)))
