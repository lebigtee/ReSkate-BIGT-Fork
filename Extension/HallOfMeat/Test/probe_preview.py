# Picks a few skeleton poses from `meat probe` recordings for HomPreview.cs: the skater riding just
# before each bail, then a few moments into it. One line a pose: a label, then the 27 points.
# usage: py probe_preview.py <bail_*.bin> ... > poses.txt
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_analyse as pa

for path in sys.argv[1:]:
    frames = list(pa.frames(path))
    start = next((n for n, f in enumerate(frames) if f['phase'] == 1), None)
    if start is None: continue
    t0 = frames[start]['time']
    name = os.path.basename(path).split('_')[1]
    picks = [('b%s riding' % name, frames[max(0, start - 20)])]
    for after in (0.4, 1.0, 2.0):
        f = next((f for f in frames[start:] if f['time'] - t0 >= after), None)
        if f: picks.append(('b%s +%.1fs' % (name, after), f))
    for label, f in picks:
        print(label.replace(' ', '_'), ' '.join('%.4f %.4f %.4f' % tuple(p) for p in f['points']))
