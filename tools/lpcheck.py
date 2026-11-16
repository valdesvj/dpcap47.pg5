#!/usr/bin/env python3
"""lpcheck.py DPTEST FILE.DPV - check DPCAP47's allocation against an exact linear programme (scipy).

For every direction (10 degrees), every case (intact, each group lost) and every Beaufort number, the LP finds the
largest lam for which lam x the force is inside the thrusters' sets (azimuth discs as 64-gons, a forbidden zone as two
convex halves, a propeller as ahead-with-rudder or astern), each combination of pieces tried. The engine's answer
(HELD) must agree with lam >= 1 but for the cases within 1 % of the edge. Power limits (gen=) are not in the LP:
leave them out of the file. Exit 1 if they disagree anywhere.
"""
import itertools
import math
import subprocess
import sys

import numpy as np
from scipy.optimize import linprog


def ask(dptest, f, case, frm, bf):
    out = subprocess.run([dptest, f, str(case), str(frm), str(bf)], capture_output=True, text=True).stdout.split("\n")
    tau = held = None
    th = []
    for ln in out:
        p = ln.split()
        if not p:
            continue
        if p[0] == "TAU":
            tau = list(map(float, p[1:]))
        elif p[0] == "T":
            th.append([int(p[1])] + list(map(float, p[2:])))
        elif p[0] == "HELD":
            held = int(p[1])
    return tau, th, held


def wrap(a):
    return a % 360.0


def lam(tau, th):
    def pieces(t):
        ty, x, y, tf, tr, cl, fa, fb = t
        if ty == 3:
            return 2 if tr > 0 else 1
        if ty in (1, 2) and wrap(fb - fa) > 0:
            return 2
        return 1

    n = len(th)
    choice = [range(pieces(t)) for t in th]
    best = 0.0
    for combo in itertools.product(*choice):
        A_eq = np.zeros((3, 2 * n + 1))
        A_ub, b_ub, bounds = [], [], []
        for i, t in enumerate(th):
            ty, x, y, tf, tr, cl, fa, fb = t
            A_eq[0, 2 * i] = 1
            A_eq[1, 2 * i + 1] = 1
            A_eq[2, 2 * i] = -y
            A_eq[2, 2 * i + 1] = x

            def row(cx, cy, b):
                r = [0.0] * (2 * n + 1)
                r[2 * i], r[2 * i + 1] = cx, cy
                A_ub.append(r)
                b_ub.append(b)

            if ty == 0:
                bounds += [(0, 0), (-tf, tf)]
            elif ty == 3:
                if combo[i] == 0:
                    bounds += [(0, tf), (None, None)]
                    row(-cl, 1, 0)
                    row(-cl, -1, 0)
                else:
                    bounds += [(-tr, 0), (0, 0)]
            else:
                bounds += [(None, None), (None, None)]
                span = wrap(fb - fa)
                if span > 0:                       # a convex half of what is allowed: two half-planes and the arc
                    half = (360 - span) / 2
                    a0 = fb + (0 if combo[i] == 0 else half)
                    a1 = a0 + half
                    for a, s in ((a0, 1), (a1, -1)):  # stay on the inside of each boundary ray
                        r = math.radians(a)
                        row(s * math.sin(r), -s * math.cos(r), 0)
                    ks = [a0 + half * k / 32 for k in range(33)]
                else:
                    ks = [360 * k / 64 for k in range(64)]
                for a in ks:
                    r = math.radians(a)
                    row(math.cos(r), math.sin(r), tf)
        A_eq[:, 2 * n] = -np.array(tau)
        bounds.append((0, None))
        c = np.zeros(2 * n + 1)
        c[-1] = -1
        res = linprog(c, A_ub=A_ub or None, b_ub=b_ub or None, A_eq=A_eq, b_eq=[0, 0, 0], bounds=bounds)
        if res.status == 0:
            best = max(best, res.x[-1])
    return best


def main():
    dptest, f = sys.argv[1], sys.argv[2]
    _, th0, _ = ask(dptest, f, 0, 0, 1)
    # the cases: intact and each group, as dptest numbers them (0 .. groups); stop at the first empty one
    cases = [0]
    for g in range(1, 7):
        _, th, _ = ask(dptest, f, g, 0, 1)
        if len(th) < len(th0):
            cases.append(g)
    bad = edge = total = 0
    for case in cases:
        for frm in range(0, 360, 10):
            for bf in range(1, 12):
                tau, th, held = ask(dptest, f, case, frm, bf)
                l = lam(tau, th)
                total += 1
                if abs(l - 1) < 0.01:
                    edge += 1
                    continue
                if (l >= 1) != bool(held):
                    bad += 1
                    print("DIFFER case %d from %3d BF %2d: LP lam %.3f, engine %s" % (case, frm, bf, l,
                          "held" if held else "not held"))
    print("%s: %d of %d agree (%d within 1 %% of the edge, not judged)" % (f, total - bad - edge, total - edge, edge))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
