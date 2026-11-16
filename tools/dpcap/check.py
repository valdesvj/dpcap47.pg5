#!/usr/bin/env python3
"""check.py - the PC program's methods checked (make pccheck). Each check prints ok or FAIL; exit 1 on a FAIL.
Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later."""
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from dpcap import engine as E          # noqa: E402
from dpcap import methods as M         # noqa: E402

fails = 0


def check(ok, msg):
    global fails
    print(("ok   " if ok else "FAIL ") + msg)
    fails += 0 if ok else 1


root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
text = open(os.path.join(root, "vessels", "EXAMPLE.DPV")).read()
v = E.parse(text)
ex = M.Extras(text)

# 1. the Python force model is the C's (level 1, provisional coefficients)
worst = 0
for frm in range(0, 360, 15):
    for bf in (3, 6, 9):
        env = E.table21()[bf]
        a = E.force_l1(v, env, frm)
        b = M.weather_force(v, None, env, frm, 1.25)
        worst = max(worst, max(abs(x - y) / (1 + abs(x)) for x, y in zip(a, b)))
check(worst < 1e-3, "the Python weather force is the C's (float precision: worst relative difference %.1e)" % worst)

# 2. level 2 with no tables of its own and factor 1.25 is level 1: the same numbers, winds within 0.1 m/s
t0 = time.time()
l1 = M.dnv_l1(v, ex)
t1 = time.time()
l2 = M.dnv_l2(v, ex, 1.25)
t2 = time.time()
dw = max(abs(a - b) for c1, c2 in zip(l1.cases, l2.cases) for a, b in zip(c1.winds, c2.winds))
check(l1.numbers == l2.numbers and dw < 0.1,
      "level 2 without tables = level 1: %s vs %s, winds within %.3f m/s (%.2f s, %.2f s)"
      % (l1.summary, l2.summary, dw, t1 - t0, t2 - t1))

# 3. coefficient tables equal to the provisional formulas give the same answer
lines = []
for d in range(0, 190, 10):
    c, s = math.cos(math.radians(d)), math.sin(math.radians(d))
    lines.append("cw=%d,%.6f,%.6f,%.6f" % (d, -M.CXW * c, -M.CYW * s, -M.CYW * s * v.xlw / v.lpp))
    lines.append("cc=%d,%.6f,%.6f,%.6f" % (d, -M.CXC * c, -M.CYC * s, -M.CYC * s * v.xlc / v.lpp))
    lines.append("cd=%d,%.6f,%.6f,0" % (d, -M.CXD * c * v.b / v.lpp / 16, -M.CYD * s / 16))
ex2 = M.Extras("\n".join(lines))
F1 = M.weather_force(v, None, E.table21()[7], 40, 1.25)
F2 = M.weather_force(v, ex2, E.table21()[7], 40, 1.25)
F3 = M.weather_force(v, ex2, E.table21()[7], 320, 1.25)
F4 = M.weather_force(v, None, E.table21()[7], 320, 1.25)
d12 = max(abs(a - b) for a, b in zip(F1, F2))
d34 = max(abs(a - b) for a, b in zip(F3, F4))
check(d12 < 0.02 * max(abs(x) for x in F1) and d34 < 0.02 * max(abs(x) for x in F3),
      "tables of the provisional coefficients (0..180, mirrored) give the same force (%.1f, %.1f kN)" % (d12, d34))

# 4. ABS: Table 1, the bollard pull formula, the SKP numbers
check(abs(M.abs_p(10) - 73.3) < 1e-9 and abs(M.abs_p(11.25) - (73.3 + 85.2) / 2) < 1e-9 and M.abs_p(40) == 100,
      "ABS Table 1: P(10 m/s) 73.3 %, between steps linear, above 35 m/s 100 %")
w, notes = M.abs_thrust(v, 0)
T0 = 1250 * (3000 * 3.6) ** (2 / 3) / 1000
check(abs(w.thr[3].tf - T0) < 0.5, "ABS bollard pull, ducted 3000 kW D 3.6 m: %.0f kN (DNV level 1 gives %.0f)"
      % (w.thr[3].tf, v.thr[3].tf))
t0 = time.time()
ab = M.abs_skp(v, ex)
t1 = time.time()
a, b, c, d = ab.skp
check(100 >= a >= b >= c >= 0 and d is not None and a >= d, "%s (%.2f s, %d allocations)" % (ab.summary, t1 - t0, ab.solves))
weak = M.abs_skp(v, ex, current_kn=3.0)
check(weak.skp[0] <= a, "more current, less SKP: %s" % weak.summary)

# 5. level 2-site: a site's weather
site = M.Extras("site=5,1,5\nsite=10,2.5,7\nsite=15,4,8.5\nsite=20,5.5,10\nsite=25,7.5,11\nsitecur=0.5\n")
st = M.dnv_l2(v, site, 1.25, site=True)
check(st.numbers is None and all(0 <= x <= 40 for x in st.cases[0].winds), "level 2-site: plots only; " + st.summary)

for r in (l1, l2, ab):
    print("  %-26s %s" % (r.method, r.summary))
print("all ok" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)
