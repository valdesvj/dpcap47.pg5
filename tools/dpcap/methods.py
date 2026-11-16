"""methods.py - the capability methods of DPCAP47's PC program, side by side.

  dnv_l1    DNV-ST-0111 level 1: the C engine itself (the calculator's numbers)
  dnv_l2    DNV-ST-0111 level 2: Table 2-1, the vessel's own coefficient tables (cw= cc= cd=) where the file has
            them, a dynamic factor of your choice; level 2-site: a site's wind/wave table and current (site= sitecur=)
  abs_skp   ABS Guide for Dynamic Positioning Systems, Section 9 (station keeping performance, SKP notation):
            the North Sea wind-wave table (Table 1), a fixed current, a 20 % margin, the ABS bollard pull formula
            where the file gives propeller diameters, and SKP(a, b, c, d, e, f)

Each returns a Result: the cases (intact, failures) with the limiting wind a direction, and a summary.
The thrust allocation is always the C engine's (dp_alloc), so only the weather, the thrust and the rules differ.
Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
"""
import math

from . import engine as E

RHO_AIR, RHO_W, G = 1.226, 1026.0, 9.81
KN = 0.514444                                    # m/s a knot

# PROVISIONAL coefficients, the same as dp_force in src/dpcap.c (level 1)
CXW, CYW, CXC, CYC, CXD, CYD = 0.80, 0.90, 0.60, 0.80, 0.30, 0.20

# ABS DPS Guide, Section 9, Table 1: North Sea wind-wave relationship. (Hs m, Tp s, Vw m/s 1-min mean, P non-exc %)
ABS_TABLE1 = [(0, 0, 0, 0.0), (1.28, 5.3, 2.5, 14.3), (1.78, 6.26, 5, 35.7), (2.44, 7.32, 7.5, 56.9),
              (3.21, 8.41, 10, 73.3), (4.09, 9.49, 12.5, 85.2), (5.07, 10.56, 15, 92.5), (6.12, 11.61, 17.5, 96.6),
              (7.26, 12.64, 20, 98.4), (8.47, 13.65, 22.5, 99.3), (9.75, 14.65, 25, 99.7), (11.09, 15.62, 27.5, 99.9),
              (12.5, 16.58, 30, 99.9), (13.97, 17.53, 32.5, 100.0), (15.49, 18.46, 35, 100.0)]


# ------------------------------------------------------------------ the vessel file's level 2 data
class Extras:
    """cw=dir,Cx,Cy,Cn  wind    Fx = q AF Cx, Fy = q AL Cy, Mz = q AL Lpp Cn   (q = rho_air V^2 / 2)
       cc=dir,Cx,Cy,Cn  current Fx = q B T Cx, Fy = q ALC Cy, Mz = q ALC Lpp Cn (q = rho_w Vc^2 / 2)
       cd=dir,Cx,Cy,Cn  drift   Fx = rho g Hs^2 Lpp Cx, Fy = .. Lpp Cy, Mz = .. Lpp^2 Cn
       The force on the vessel in its axes (x ahead, y starboard; a head wind has Cx < 0); dir: where it comes
       from, 0 = ahead, clockwise. Given for 0..180 only, the other side is mirrored.
       site=wind,Hs,Tp  a site's weather, one line a step (wind m/s, 10-min mean)    sitecur=m/s"""

    def __init__(self, text=""):
        self.cw, self.cc, self.cd, self.site, self.sitecur = {}, {}, {}, [], None
        for ln in text.splitlines():
            ln = ln.split("#")[0].strip()
            if "=" not in ln:
                continue
            k, v = (s.strip() for s in ln.split("=", 1))
            k = k.lower()
            try:
                f = [float(x) for x in v.split(",")]
            except ValueError:
                continue
            if k in ("cw", "cc", "cd") and len(f) == 4:
                getattr(self, k)[f[0] % 360] = tuple(f[1:])
            elif k == "site" and len(f) == 3:
                self.site.append(tuple(f))
            elif k == "sitecur" and f:
                self.sitecur = f[0]
        self.site.sort()
        for t in (self.cw, self.cc, self.cd):
            if t and max(t) <= 180:
                for d, (cx, cy, cn) in list(t.items()):
                    if 0 < d < 180:
                        t[360 - d] = (cx, -cy, -cn)

    def level2(self):
        return bool(self.cw or self.cc or self.cd)


def _coef(table, d):
    """A coefficient table at direction d, linear between its points (wrapping round)."""
    ks = sorted(table)
    d %= 360
    lo = max([k for k in ks if k <= d], default=ks[-1] - 360)
    hi = min([k for k in ks if k > d], default=ks[0] + 360)
    a, b = table[lo % 360], table[hi % 360]
    t = (d - lo) / (hi - lo) if hi != lo else 0
    return tuple(a[i] + t * (b[i] - a[i]) for i in range(3))


def weather_force(v, ex, env, frm, factor):
    """The weather's force on the vessel (Fx, Fy, Mz) x factor: the vessel's own tables where it has them,
    else the provisional coefficients (as the C's dp_force)."""
    wind, hs, _tp, cur = env
    c, s = math.cos(math.radians(frm)), math.sin(math.radians(frm))
    qw = 0.5 * RHO_AIR * wind * wind / 1000
    qc = 0.5 * RHO_W * cur * cur / 1000
    qd = RHO_W * G * hs * hs / 1000
    F = [0.0, 0.0, 0.0]
    if ex is not None and ex.cw:
        cx, cy, cn = _coef(ex.cw, frm)
        F[0] += qw * v.afw * cx; F[1] += qw * v.alw * cy; F[2] += qw * v.alw * v.lpp * cn
    else:
        wy = -qw * v.alw * CYW * s
        F[0] += -qw * v.afw * CXW * c; F[1] += wy; F[2] += wy * v.xlw
    if ex is not None and ex.cc:
        cx, cy, cn = _coef(ex.cc, frm)
        F[0] += qc * v.b * v.t * cx; F[1] += qc * v.alc * cy; F[2] += qc * v.alc * v.lpp * cn
    else:
        cy_ = -qc * v.alc * CYC * s
        F[0] += -qc * v.b * v.t * CXC * c; F[1] += cy_; F[2] += cy_ * v.xlc
    if ex is not None and ex.cd:
        cx, cy, cn = _coef(ex.cd, frm)
        F[0] += qd * v.lpp * cx; F[1] += qd * v.lpp * cy; F[2] += qd * v.lpp * v.lpp * cn
    else:
        F[0] += -qd / 16 * v.b * CXD * c; F[1] += -qd / 16 * v.lpp * CYD * s
    return tuple(factor * x for x in F)


def interp_env(table, w, cur):
    """table rows (wind, hs, tp, cur or None) by wind; cur, if not None, is the current for every wind."""
    if w <= table[0][0]:
        r = table[0]
        return (w, r[1], r[2], cur if cur is not None else r[3])
    for a, b in zip(table, table[1:]):
        if w <= b[0]:
            t = (w - a[0]) / (b[0] - a[0])
            c_ = cur if cur is not None else a[3] + t * (b[3] - a[3])
            return (w, a[1] + t * (b[1] - a[1]), a[2] + t * (b[2] - a[2]), c_)
    r = table[-1]
    return (w, r[1], r[2], cur if cur is not None else r[3])


# ------------------------------------------------------------------ results
class Case:
    def __init__(self, label, mask, winds, nums=None, kind="intact"):
        self.label, self.mask, self.winds, self.nums, self.kind = label, mask, winds, nums, kind


class Result:
    def __init__(self, method, wind_def):
        self.method, self.wind_def = method, wind_def
        self.cases, self.summary, self.notes = [], "", []
        self.solves = 0

    def case(self, kind):
        return next((c for c in self.cases if c.kind == kind), None)


def _search(held, rows):
    """The number (the last row held, counting up) and the limiting wind: held(w) -> bool, monotone in w.
    rows: the winds of the table's steps (rows[0] = 0)."""
    if not held(rows[0] + 1e-6):
        return 0, 0.0
    n = 0
    for i in range(1, len(rows)):
        if held(rows[i]):
            n = i
        else:
            break
    lo = rows[n]
    hi = rows[n + 1] if n + 1 < len(rows) else 40.0
    if n + 1 >= len(rows) and held(hi):
        return n, hi
    for _ in range(12):
        mid = (lo + hi) / 2
        if held(mid):
            lo = mid
        else:
            hi = mid
    return n, lo


def _limits(v, ex, mask, rows, env_of, factor):
    a = E.Alloc()
    nums, winds, count = [], [], [0]

    def held(w, frm=0.0):
        F = weather_force(v, ex, env_of(w), frm, factor)
        count[0] += 1
        return E.alloc(v, mask, (-F[0], -F[1], -F[2]), a).ok

    for h in range(E.NHEAD):
        n, w = _search(lambda w: held(w, h * 10.0), rows)
        nums.append(n)
        winds.append(w)
    return nums, winds, count[0]


def _worst(cases, n):
    return [min(c.winds[h] for c in cases) for h in range(n)]


def _abcd(intact, wcsf):
    bow = [h for h in range(E.NHEAD) if h <= 3 or h >= E.NHEAD - 3]
    A = min(intact[h] for h in bow)
    B = min(intact)
    if wcsf is None:
        return A, B, None, None
    return A, B, min(wcsf[h] for h in bow), min(wcsf)


def _fmt(n):
    return "NA" if n is None else str(n)


# ------------------------------------------------------------------ DNV level 1: the C engine
def dnv_l1(v, ex=None):
    r = E.analyse_l1(v)
    res = Result("DNV-ST-0111 level 1", "10-min mean")
    res.cases.append(Case("intact", E.case_mask(v, 0), list(r.wind[0]), list(r.num[0]), "intact"))
    for g in range(1, r.ncase):
        res.cases.append(Case("group %d lost" % g, E.case_mask(v, g), list(r.wind[g]), list(r.num[g]), "group"))
    if r.ncase > 1:
        res.cases.append(Case("worst single failure", 0, list(r.wwind), list(r.wnum), "wcsf"))
    D = lambda n: None if n < 0 else n
    res.summary = "DP Capability-L1 (%s, %s, %s, %s)" % (r.A, r.B, _fmt(D(r.C)), _fmt(D(r.D)))
    res.numbers = (r.A, r.B, D(r.C), D(r.D))
    res.solves = r.solves
    res.notes = ["The calculator's engine and numbers.",
                 "Wind, current and wave drift coefficients and kN/kW are PROVISIONAL (not yet DNV's formulas)."]
    return res


# ------------------------------------------------------------------ DNV level 2 and level 2-site
def dnv_l2(v, ex, dyn=1.25, site=False):
    t21 = E.table21()
    if site:
        if not ex or not ex.site:
            raise ValueError("level 2-site needs the site's weather: site=wind,Hs,Tp lines (and sitecur=)")
        table = [(0.0, 0.0, 0.0, ex.sitecur or 0.0)] + [(w, h, t, ex.sitecur or 0.0) for w, h, t in ex.site]
        rows = [r[0] for r in table]
        cur = ex.sitecur or 0.0
        name = "DNV-ST-0111 level 2-site"
    else:
        table, rows, cur = t21, [r[0] for r in t21], None
        name = "DNV-ST-0111 level 2"
    env_of = lambda w: interp_env(table, w, cur)
    res = Result(name, "10-min mean")
    groups = sorted({t.grp for t in E.thrusters(v)})
    cases = [("intact", E.case_mask(v, 0), "intact")] + \
            [("group %d lost" % g, E.case_mask(v, g), "group") for g in groups if len(groups) > 1]
    for label, m, kind in cases:
        nums, winds, n = _limits(v, ex, m, rows, env_of, dyn)
        res.solves += n
        res.cases.append(Case(label, m, winds, None if site else nums, kind))
    gl = [c for c in res.cases if c.kind == "group"]
    if gl:
        res.cases.append(Case("worst single failure", 0, _worst(gl, E.NHEAD),
                              None if site else [min(c.nums[h] for c in gl) for h in range(E.NHEAD)], "wcsf"))
    src = "the vessel's tables" if ex and ex.level2() else "the PROVISIONAL level 1 coefficients (no cw/cc/cd)"
    res.notes = ["Forces from %s; dynamic factor %.2f." % (src, dyn)]
    if site:
        res.summary = "site: current %.2f m/s, %d weather steps (plots only, no numbers)" % (cur, len(ex.site))
        res.numbers = None
    else:
        w = res.case("wcsf")
        A, B, Cn, Dn = _abcd(res.cases[0].nums, w.nums if w else None)
        res.numbers = (A, B, Cn, Dn)
        res.summary = "DP Capability-L2 (%s, %s, %s, %s)" % (A, B, _fmt(Cn), _fmt(Dn))
    return res


# ------------------------------------------------------------------ ABS SKP
def abs_thrust(v, cur_ms):
    """A copy of the vessel with the ABS bollard pull where a diameter is given: T0 = K (P D)^(2/3) N, K 848 open,
    1250 ducted (P kW, D m), less the current: EFC = 1 - (K1 / PA)^n K2 Vc (PA kW/m2 of the disc).
    Tunnels and thrusters without a diameter keep the DNV level 1 values. Returns (vessel, notes)."""
    w = E.copy(v)
    notes = []
    for i in range(w.nthr):
        t = w.thr[i]
        if t.type == E.T_TUN or t.d <= 0 or t.kw <= 0:
            continue
        ducted = t.type == E.T_AZD or (t.type == E.T_PROP and t.duct)
        K, K1, n = (1250, 400, 0.25) if ducted else (848, 10, 0.28)
        T0 = K * (t.kw * t.d) ** (2 / 3) / 1000.0
        pa = t.kw / (math.pi * t.d * t.d / 4)
        efc = max(0.0, 1 - (K1 / pa) ** n * 0.11 * cur_ms)
        ratio = t.tr / t.tf if t.tf > 0 else 0
        t.tf = T0 * efc
        t.tr = t.tf * ratio
        notes.append("%s: %.0f kN (ABS, current %.0f %%)" % (t.name.decode(), t.tf, efc * 100))
    return w, notes


def abs_p(w):
    """P non-exceedance (%) of a 1-min wind in ABS Table 1."""
    T = ABS_TABLE1
    if w >= T[-1][2]:
        return 100.0
    for a, b in zip(T, T[1:]):
        if w <= b[2]:
            return a[3] + (w - a[2]) / (b[2] - a[2]) * (b[3] - a[3])
    return 100.0


def abs_skp(v, ex, current_kn=1.5, margin=0.20, use_abs_thrust=True, location="North Sea"):
    cur = current_kn * KN
    w, tnotes = abs_thrust(v, cur) if use_abs_thrust else (v, [])
    table = [(r[2], r[0], r[1], cur) for r in ABS_TABLE1]
    rows = [r[0] for r in table]
    env_of = lambda x: interp_env(table, x, cur)
    res = Result("ABS DPS Guide SKP", "1-min mean")
    skp = lambda winds: sum(abs_p(x) for x in winds) / len(winds)
    _n, iw, n = _limits(w, ex, E.case_mask(w, 0), rows, env_of, 1 + margin)
    res.solves += n
    res.cases.append(Case("intact (a)", E.case_mask(w, 0), iw, None, "intact"))
    singles = []
    for i in range(w.nthr):
        m = E.case_mask(w, 0) & ~(1 << i)
        _n, sw, n = _limits(w, ex, m, rows, env_of, 1 + margin)
        res.solves += n
        singles.append(Case("%s lost" % w.thr[i].name.decode(), m, sw, None, "single"))
    groups = sorted({t.grp for t in E.thrusters(w)})
    gcases = []
    if len(groups) > 1:
        for g in groups:
            m = E.case_mask(w, g)
            _n, gw, n = _limits(w, ex, m, rows, env_of, 1 + margin)
            res.solves += n
            gcases.append(Case("group %d lost" % g, m, gw, None, "group"))
    a = skp(iw)
    sk = [(skp(c.winds), c) for c in singles]
    bmin = max(sk, key=lambda x: x[0]) if sk else None
    cmax = min(sk, key=lambda x: x[0]) if sk else None
    if bmin:
        bmin[1].kind, bmin[1].label = "min-effect", "min-effect: " + bmin[1].label + " (b)"
        cmax[1].kind, cmax[1].label = "max-effect", "max-effect: " + cmax[1].label + " (c)"
        res.cases += [bmin[1], cmax[1]] if bmin[1] is not cmax[1] else [cmax[1]]
    d = None
    if gcases:
        gd = min(((skp(c.winds), c) for c in gcases), key=lambda x: x[0])
        d = gd[0]
        res.cases += gcases
        res.cases.append(Case("worst case failure: " + gd[1].label + " (d)", gd[1].mask, gd[1].winds, None, "wcsf"))
    b = bmin[0] if bmin else None
    c = cmax[0] if cmax else None
    f = lambda x: "-" if x is None else "%.0f" % x
    res.skp = (a, b, c, d)
    res.summary = "SKP(%s, %s, %s, %s, %.1f, %s)" % (f(a), f(b), f(c), f(d), current_kn, location)
    res.notes = ["Wind is the 1-min mean of ABS Table 1 (North Sea); DNV's is the 10-min mean.",
                 "Margin %.0f %% on the weather (the DPS Guide's quasi-static safety margin)." % (margin * 100),
                 "Current %.2f kn (%.2f m/s) from every direction with the wind." % (current_kn, cur)] + tnotes
    if not tnotes:
        res.notes.append("No propeller diameters (thr=...,D): thrust as DNV level 1.")
    return res
