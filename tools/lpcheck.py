#!/usr/bin/env python3
"""lpcheck.py DPTEST FILE.DPV - check DPCAP47's allocation against an exact linear programme (scipy).

For every direction (10 degrees), every case (intact, each group lost) and every Beaufort number, dptest gives the
force to hold and each working thruster's set as the engine has it: a union of convex pieces (segments, discs,
sectors, polygons: rudders, forbidden and flushing zones, skeg losses) and its ventilation loss. The LP finds the
largest lam for which lam x the force is reachable, trying each combination of pieces (discs and arcs as 64-gons).
The engine's answer (HELD) must agree with lam >= 1 but for the cases within 1 % of the edge. Power limits (gen=)
are not in the LP: leave them out of the file. Exit 1 if they disagree anywhere.
Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
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
            th.append({"x": float(p[2]), "y": float(p[3]), "beta": float(p[4]), "pieces": []})
        elif p[0] == "P":
            kind = int(p[1])
            a0, span, r = float(p[2]), float(p[3]), float(p[4])
            v = list(map(float, p[5:]))
            th[-1]["pieces"].append((kind, a0, span, r, [(v[k], v[k + 1]) for k in range(0, len(v), 2)]))
        elif p[0] == "HELD":
            held = int(p[1])
    return tau, th, held


def lam(tau, th):
    n = len(th)
    best = 0.0
    for combo in itertools.product(*[range(len(t["pieces"])) for t in th]):
        A_eq = np.zeros((3, 2 * n + 1))
        A_ub, b_ub, eq_rows, eq_b = [], [], [], []
        for i, t in enumerate(th):
            A_eq[0, 2 * i] = 1
            A_eq[1, 2 * i + 1] = 1
            A_eq[2, 2 * i] = -t["y"]
            A_eq[2, 2 * i + 1] = t["x"]
            s = t["beta"]
            kind, a0, span, r, vs = t["pieces"][combo[i]]

            def le(cx, cy, b):                          # cx fx + cy fy <= b
                row = [0.0] * (2 * n + 1)
                row[2 * i], row[2 * i + 1] = cx, cy
                A_ub.append(row)
                b_ub.append(b)

            if kind == 0:                               # a segment: on its line, between its ends
                (ax, ay), (bx, by) = [(x * s, y * s) for x, y in vs]
                dx, dy = bx - ax, by - ay
                row = [0.0] * (2 * n + 1)
                row[2 * i], row[2 * i + 1] = -dy, dx
                eq_rows.append(row)
                eq_b.append(-dy * ax + dx * ay)
                le(-dx, -dy, -(dx * ax + dy * ay))
                le(dx, dy, dx * bx + dy * by)
            elif kind == 1:                             # a disc
                for k in range(64):
                    a = 2 * math.pi * k / 64
                    le(math.cos(a), math.sin(a), r * s)
            elif kind == 2:                             # a sector: two half-planes and the arc
                for a, sg in ((a0, 1), (a0 + span, -1)):
                    q = math.radians(a)
                    le(sg * math.sin(q), -sg * math.cos(q), 0)
                for k in range(33):
                    q = math.radians(a0 + span * k / 32)
                    le(math.cos(q), math.sin(q), r * s)
            else:                                       # a convex polygon, corners in order
                m = len(vs)
                for k in range(m):
                    (ax, ay), (bx, by) = vs[k], vs[(k + 1) % m]
                    ax, ay, bx, by = ax * s, ay * s, bx * s, by * s
                    # inside: (b - a) x (f - a) >= 0
                    le((by - ay), -(bx - ax), (by - ay) * ax - (bx - ax) * ay)
        A_eq[:, 2 * n] = -np.array(tau)
        Aeq = np.vstack([A_eq] + [np.array(eq_rows)] if eq_rows else [A_eq])
        beq = [0, 0, 0] + eq_b
        bounds = [(None, None)] * (2 * n) + [(0, None)]
        c = np.zeros(2 * n + 1)
        c[-1] = -1
        res = linprog(c, A_ub=A_ub or None, b_ub=b_ub or None, A_eq=Aeq, b_eq=beq, bounds=bounds)
        if res.status == 0:
            best = max(best, res.x[-1])
    return best


def main():
    dptest, f = sys.argv[1], sys.argv[2]
    _, th0, _ = ask(dptest, f, 0, 0, 1)
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
                lm = lam(tau, th)
                total += 1
                if abs(lm - 1) < 0.01:
                    edge += 1
                    continue
                if (lm >= 1) != bool(held):
                    bad += 1
                    print("DIFFER case %d from %3d BF %2d: LP lam %.3f, engine %s" % (case, frm, bf, lm,
                          "held" if held else "not held"))
    print("%s: %d of %d agree (%d within 1 %% of the edge, not judged)" % (f, total - bad - edge, total - edge, edge))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
