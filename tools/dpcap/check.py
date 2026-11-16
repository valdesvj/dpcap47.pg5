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

# 1. the Python force model is the C's (level 1, 3.5 - 3.7)
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

# 3. wind and current tables made from level 1's formulas (3.5, 3.6) give the same force at the table's directions
lines = []
for d in range(0, 190, 10):
    c, s, k = math.cos(math.radians(d)), math.sin(math.radians(d)), d / 180.0
    cyw = -0.9 * s
    lc = max(min(0.4 * (1 - 2 * k), 0.25), -0.2)
    lines.append("cw=%d,%.6f,%.6f,%.6f" % (d, -0.7 * c, cyw, cyw * (v.xlw / v.lpp + 0.3 * (1 - 2 * k))))
    lines.append("cc=%d,%.6f,%.6f,%.6f" % (d, -0.07 * c, -0.6 * s, -0.6 * s * (v.xlc / v.lpp + lc)))
ex2 = M.Extras("\n".join(lines))
dd = 0
for frm in (0, 40, 90, 150, 220, 320):
    F1 = M.weather_force(v, None, E.table21()[7], frm, 1.25)
    F2 = M.weather_force(v, ex2, E.table21()[7], frm, 1.25)
    dd = max(dd, max(abs(a - b) / (1 + abs(a)) for a, b in zip(F1, F2)))
check(dd < 1e-3, "tables of level 1's wind and current coefficients (0..180, mirrored) give the same force "
      "(worst relative difference %.1e)" % dd)

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

# 4b. the calculator's ABS (C) is the PC's (Python)
t0 = time.time()
ca = E.abs_c(v, 1.5, 0.20)
t1 = time.time()
dw = max(abs(x - y) for x, y in zip(ca.intact, ab.case("intact").winds))
ds = max(abs(x - y) for x, y in zip((ca.a, ca.b, ca.c, ca.d), ab.skp))
check(dw < 0.15 and ds < 0.3, "the calculator's ABS = the PC's: SKP(%.0f, %.0f, %.0f, %.0f), intact winds within %.3f m/s, "
      "SKP within %.2f (%.2f s)" % (ca.a, ca.b, ca.c, ca.d, dw, ds, t1 - t0))

# 5. level 2-site: a site's weather
site = M.Extras("site=5,1,5\nsite=10,2.5,7\nsite=15,4,8.5\nsite=20,5.5,10\nsite=25,7.5,11\nsitecur=0.5\n")
st = M.dnv_l2(v, site, 1.25, site=True)
check(st.numbers is None and all(0 <= x <= 40 for x in st.cases[0].winds), "level 2-site: plots only; " + st.summary)

for r in (l1, l2, ab):
    print("  %-26s %s" % (r.method, r.summary))
print("all ok" if not fails else "%d FAILED" % fails)
sys.exit(1 if fails else 0)
