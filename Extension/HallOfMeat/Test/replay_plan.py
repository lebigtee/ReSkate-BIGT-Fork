# Simulates the Hall of Meat replay plan (hall_of_meat_hud.cpp, plan_replay): how many of the
# slots wait to be shown at various frame rates, and how many distinct pictures reach the screen.
# Keep the constants in step with hall_of_meat_hud.cpp and hall_of_meat_replay.h. usage: py replay_plan.py
SLOTS = 24
import math
stutter = 1/40; rin, fade, end, speed = 0.12, 0.72, 1.02, 0.5
keep_every = stutter * speed
def moment(t):
    return t - (1-speed)*t*t/(2*rin) if t <= rin else rin*(1+speed)/2 + speed*(t-rin)
def run(fps, jitter=0.0):
    import random; random.seed(1)
    slots = [None]*SLOTS; ids = 0; last = -1; on_screen = 0; t = 0.0
    used_max = 0; skipped = 0; shown_moments = []; kept = 0
    while t < end:
        if t <= moment(end) + keep_every and t - last >= keep_every*0.9:
            for k in range(SLOTS):
                s = slots[k]
                if s and s[1] >= on_screen: continue
                ids += 1; slots[k] = (ids, t); last = t; kept += 1; break
            else:
                skipped += 1
        m = moment(math.floor(t/stutter)*stutter) + 1e-6
        best = None
        for s in slots:
            if s and s[1] <= m and (best is None or s[1] > best[1]): best = s
        if best: on_screen = best[1]; shown_moments.append(best[1])
        used_max = max(used_max, sum(1 for s in slots if s and s[1] >= on_screen))
        t += 1/fps * (1 + random.uniform(-jitter, jitter))
    distinct = len(set(shown_moments))
    return used_max, skipped, kept, distinct, max(shown_moments)
for fps in (30, 45, 60, 90, 144, 240):
    for j in (0.0, 0.3):
        u, sk, kept, d, last = run(fps, j)
        print(f"{fps:>3} fps jitter {j:.1f}: slots in use at most {u:2d}/{SLOTS}, pictures not kept for want of a slot {sk}, kept {kept}, distinct on screen {d}, last moment {last:.2f}s (of {moment(end):.2f})")
