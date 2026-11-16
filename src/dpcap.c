/* dpcap.c - DPCAP47's engine: the weather's forces, the thrusters, the thrust allocation and the capability search.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later. See dpcap.h for the axes.
 *
 * What follows DNV-ST-0111 level 1: Table 2-1 (wind, Hs, Tp, current a Beaufort number), wind, waves and current
 * from the same direction, the dynamic factor 1.25 on all three, the thruster efficiencies (tunnel inlet, reversed
 * propellers, mechanical efficiency, 0.9 for miscellaneous losses), the rudder angle capped at 30 degrees, 10 % of a
 * group's power kept for the ship, 10 degree headings, the numbers counted up from the lowest condition, and
 * A B C D (A, C: the worst within 30 degrees of the bow; B, D: all round; C, D: the worst single failure).
 * PROVISIONAL (not the standard's own formulas, which are to be put in from the DNV PDF, sections 3.5 - 3.7):
 * the wind, current and wave drift coefficients (dp_force) and the thrust a kW of each kind of thruster (KN_KW).
 * Not yet: thruster-thruster interaction, ventilation, period-dependent wave drift.
 *
 * The allocation: minimise |W (B f - tau)|^2 + eps |f|^2 with each thruster's force inside its own set (a tunnel's
 * segment, an azimuth's disc less its forbidden zone, a propeller's thrust with its rudder's side force), by
 * projected gradient with Nesterov's momentum (FISTA) and restarts. The force is held when the residual is under
 * 0.5 % of the demand and no group draws more power than it has. */
#include "dpcap.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define PI_F   3.14159265f
#define RAD(d) ((d) * (PI_F / 180.0f))
#define RHO_AIR 1.226f                  /* kg/m3, DNV-ST-0111 3.5 */
#define RHO_W   1026.0f                 /* kg/m3, 3.6 */
#define G       9.81f
#define TOL     0.005f                  /* held: residual under 0.5 % of the demand */
#define EPS     1e-3f

float dp_dyn = 1.25f;
uint32_t dp_solves, dp_iters;

/* Table 2-1: Beaufort, wind (upper limit, 10 min mean at 10 m), Hs, Tp, current */
const Env DP_TABLE[NBF] = {
  {  0.0f,  0.0f,  0.0f, 0.00f }, {  1.5f,  0.1f,  3.5f, 0.25f }, {  3.4f,  0.4f,  4.5f, 0.50f },
  {  5.4f,  0.8f,  5.5f, 0.75f }, {  7.9f,  1.3f,  6.5f, 0.75f }, { 10.7f,  2.1f,  7.5f, 0.75f },
  { 13.8f,  3.1f,  8.5f, 0.75f }, { 17.1f,  4.2f,  9.0f, 0.75f }, { 20.7f,  5.7f, 10.0f, 0.75f },
  { 24.4f,  7.4f, 10.5f, 0.75f }, { 28.4f,  9.5f, 11.5f, 0.75f }, { 32.6f, 12.1f, 12.0f, 0.75f },
};

const char *const DP_TYPE[T_CNT] = { "TUN", "AZI", "AZD", "PROP" };

/* PROVISIONAL: nominal bollard thrust a kW of shaft power, by kind (open/ducted propeller for PROP) */
static const float KN_KW[T_CNT] = { 0.110f, 0.130f, 0.160f, 0.130f };
#define KN_KW_PROP_DUCT 0.155f

Env dp_env_wind(float w) {
  Env e;
  if (w <= 0) { e = DP_TABLE[0]; e.wind = 0; return e; }
  int i = 1;
  while (i < NBF - 1 && DP_TABLE[i].wind < w) i++;
  const Env *a = &DP_TABLE[i - 1], *b = &DP_TABLE[i];
  float k = (w - a->wind) / (b->wind - a->wind);
  if (k > 1) k = 1;                     /* above Beaufort 11: the waves stay as at 11 */
  e.wind = w;
  e.hs = a->hs + k * (b->hs - a->hs);
  e.tp = (i == 1 ? b->tp : a->tp + k * (b->tp - a->tp));
  e.cur = a->cur + k * (b->cur - a->cur);
  return e;
}

/* PROVISIONAL coefficients: wind and current as drag on the frontal and side areas, wave drift as 1/16 rho g Hs^2
   on the length the waves meet. Each force points the way the weather goes. */
#define CXW 0.80f
#define CYW 0.90f
#define CXC 0.60f
#define CYC 0.80f
#define CXD 0.30f
#define CYD 0.20f

void dp_force(const Vessel *v, const Env *e, float from, float tau[3]) {
  float c = cosf(RAD(from)), s = sinf(RAD(from));
  float qw = 0.5f * RHO_AIR * e->wind * e->wind / 1000.0f;            /* kPa -> kN with m2 */
  float qc = 0.5f * RHO_W * e->cur * e->cur / 1000.0f;
  float qd = RHO_W * G * e->hs * e->hs / 16.0f / 1000.0f;
  float wx = -qw * v->afw * CXW * c, wy = -qw * v->alw * CYW * s;
  float cx = -qc * v->b * v->t * CXC * c, cy = -qc * v->alc * CYC * s;
  float dx = -qd * v->b * CXD * c, dy = -qd * v->lpp * CYD * s;
  tau[0] = dp_dyn * (wx + cx + dx);
  tau[1] = dp_dyn * (wy + cy + dy);
  tau[2] = dp_dyn * (wy * v->xlw + cy * v->xlc);
}

void dp_prepare(Vessel *v) {
  v->ngrp = 0;
  for (int i = 0; i < v->nthr; i++) {
    Thruster *t = &v->thr[i];
    float k = t->type == T_PROP && t->duct ? KN_KW_PROP_DUCT : KN_KW[t->type];
    float em = t->type == T_PROP ? 0.97f : 0.93f;                     /* mechanical efficiency: shaft line / tunnel, azimuth */
    float nom = t->kn > 0 ? t->kn : k * t->kw * em;
    float e1 = t->type == T_TUN ? (t->rounded ? 1.07f : 0.93f) : 1.0f; /* tunnel inlet */
    t->tf = nom * e1 * 0.9f;                                           /* 0.9: miscellaneous losses */
    t->tr = 0;
    if (t->type == T_TUN) t->tr = t->tf;
    if (t->type == T_PROP) t->tr = t->tf * (t->cpp ? (t->duct ? 0.5f : 0.65f) : (t->duct ? 0.7f : 0.9f));
    t->cl = t->type == T_PROP && t->rudder ? 0.35f : 0.0f;            /* PROVISIONAL: side force at 30 degrees */
    if (t->grp < 1) t->grp = 1;
    if (t->grp > MAX_GRP) t->grp = MAX_GRP;
    if (t->grp > v->ngrp) v->ngrp = t->grp;
  }
}

/* ------------------------------------------------------------------ each thruster's set */
static float wrap360(float a) { a = fmodf(a, 360.0f); return a < 0 ? a + 360.0f : a; }

static void ray(float ang, float R, const float p[2], float o[2]) {   /* nearest point of the ray 0..R at ang */
  float ux = cosf(RAD(ang)), uy = sinf(RAD(ang));
  float t = p[0] * ux + p[1] * uy;
  if (t < 0) t = 0;
  if (t > R) t = R;
  o[0] = t * ux; o[1] = t * uy;
}

static void seg(const float a[2], const float b[2], const float p[2], float o[2]) {
  float dx = b[0] - a[0], dy = b[1] - a[1], l = dx * dx + dy * dy;
  float t = l > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l : 0;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  o[0] = a[0] + t * dx; o[1] = a[1] + t * dy;
}

static float d2(const float a[2], const float b[2]) { float x = a[0] - b[0], y = a[1] - b[1]; return x * x + y * y; }

static void project(const Thruster *t, float f[2]);

static int pieces(const Thruster *t) {                  /* a set not convex comes in two convex pieces */
  if (t->type == T_PROP) return t->tr > 0 ? 2 : 1;
  if ((t->type == T_AZI || t->type == T_AZD) && wrap360(t->fz_b - t->fz_a) > 0) return 2;
  return 1;
}

static void sector(float a0, float span, float R, float f[2]) {   /* the disc's sector a0 .. a0 + span (<= 180) */
  float r2 = f[0] * f[0] + f[1] * f[1];
  if (r2 < 1e-12f) return;
  float th = wrap360(atan2f(f[1], f[0]) * (180.0f / PI_F));
  if (wrap360(th - a0) <= span) {
    if (r2 > R * R) { float k = R / sqrtf(r2); f[0] *= k; f[1] *= k; }
    return;
  }
  float o1[2], o2[2];
  ray(a0, R, f, o1);
  ray(a0 + span, R, f, o2);
  if (d2(o1, f) <= d2(o2, f)) { f[0] = o1[0]; f[1] = o1[1]; } else { f[0] = o2[0]; f[1] = o2[1]; }
}

/* piece -1: the whole set; 0, 1: one convex piece (PROP: ahead with the rudder / astern; azimuth with a forbidden
   zone: the two halves of what is allowed) */
static void project_piece(const Thruster *t, float f[2], int piece) {
  if (piece >= 0 && t->type == T_PROP) {
    if (piece == 1) { f[1] = 0; if (f[0] > 0) f[0] = 0; if (f[0] < -t->tr) f[0] = -t->tr; return; }
    if (t->cl <= 0) { f[1] = 0; if (f[0] < 0) f[0] = 0; if (f[0] > t->tf) f[0] = t->tf; return; }
    if (f[0] >= 0 && f[0] <= t->tf && fabsf(f[1]) <= t->cl * f[0]) return;
    static const float O[2] = { 0, 0 };
    float A[2] = { t->tf, t->cl * t->tf }, B[2] = { t->tf, -t->cl * t->tf }, best[2], o[2], bd;
    seg(O, A, f, best); bd = d2(best, f);
    seg(A, B, f, o); if (d2(o, f) < bd) { bd = d2(o, f); best[0] = o[0]; best[1] = o[1]; }
    seg(B, O, f, o); if (d2(o, f) < bd) { best[0] = o[0]; best[1] = o[1]; }
    f[0] = best[0]; f[1] = best[1];
    return;
  }
  if (piece >= 0 && (t->type == T_AZI || t->type == T_AZD)) {
    float half = (360.0f - wrap360(t->fz_b - t->fz_a)) / 2;
    sector(piece == 0 ? t->fz_b : t->fz_b + half, half, t->tf, f);
    return;
  }
  project(t, f);
}

static void project(const Thruster *t, float f[2]) {
  if (t->type == T_TUN) {
    f[0] = 0;
    if (f[1] > t->tf) f[1] = t->tf;
    if (f[1] < -t->tf) f[1] = -t->tf;
    return;
  }
  if (t->type == T_PROP) {
    if (t->cl <= 0) {
      f[1] = 0;
      if (f[0] > t->tf) f[0] = t->tf;
      if (f[0] < -t->tr) f[0] = -t->tr;
      return;
    }
    float side = t->cl * t->tf;
    if (f[0] >= 0 && f[0] <= t->tf && fabsf(f[1]) <= t->cl * f[0]) return;          /* in the rudder's triangle */
    static const float O[2] = { 0, 0 };
    float A[2] = { t->tf, side }, B[2] = { t->tf, -side }, R[2] = { -t->tr, 0 };
    float best[2], o[2], bd;
    seg(O, A, f, best); bd = d2(best, f);
    seg(A, B, f, o); if (d2(o, f) < bd) { bd = d2(o, f); best[0] = o[0]; best[1] = o[1]; }
    seg(B, O, f, o); if (d2(o, f) < bd) { bd = d2(o, f); best[0] = o[0]; best[1] = o[1]; }
    seg(O, R, f, o); if (d2(o, f) < bd) { best[0] = o[0]; best[1] = o[1]; }       /* astern, no rudder force */
    f[0] = best[0]; f[1] = best[1];
    return;
  }
  /* azimuth: a disc, less the forbidden zone */
  float R = t->tf, r2 = f[0] * f[0] + f[1] * f[1];
  if (r2 < 1e-12f) return;
  float span = wrap360(t->fz_b - t->fz_a);
  if (span > 0) {
    float th = wrap360(atan2f(f[1], f[0]) * (180.0f / PI_F));
    if (wrap360(th - t->fz_a) < span) {
      float o1[2], o2[2];
      ray(t->fz_a, R, f, o1);
      ray(t->fz_b, R, f, o2);
      if (d2(o1, f) <= d2(o2, f)) { f[0] = o1[0]; f[1] = o1[1]; } else { f[0] = o2[0]; f[1] = o2[1]; }
      return;
    }
  }
  if (r2 > R * R) { float k = R / sqrtf(r2); f[0] *= k; f[1] *= k; }
}

static float capacity(const Thruster *t, const float f[2]) {
  if (t->type == T_PROP && f[0] < 0) return t->tr;
  return t->tf;
}

/* ------------------------------------------------------------------ the allocation */
static int alloc_core(const Vessel *v, uint32_t mask, const float tau[3], Alloc *a, int maxit, const signed char *pc) {
  int idx[MAX_THR], n = 0;
  for (int i = 0; i < v->nthr; i++) {
    if (mask >> i & 1) idx[n++] = i;
    else a->f[i][0] = a->f[i][1] = 0;
  }
  float wm = 2.0f / v->lpp;                                            /* the moment weighed by half the length */
  float W[3] = { 1, 1, wm };
  float T[3] = { tau[0], tau[1], tau[2] };
  float tn = sqrtf(T[0] * T[0] + T[1] * T[1] + (wm * T[2]) * (wm * T[2]));
  if (n == 0) {
    a->res = tn > 1e-3f ? 1 : 0; a->ok = a->res <= TOL; a->power_ok = 1; a->iters = 0;
    for (int i = 0; i < v->nthr; i++) a->use[i] = 0;
    return a->ok;
  }
  /* Lipschitz constant: the largest eigenvalue of W B B^T W (3 x 3), by power iteration */
  float M[3][3] = { { 0 } };
  for (int k = 0; k < n; k++) {
    const Thruster *t = &v->thr[idx[k]];
    float cx[3] = { 1, 0, -t->y * wm }, cy[3] = { 0, 1, t->x * wm };
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) M[r][c] += cx[r] * cx[c] + cy[r] * cy[c];
  }
  float e[3] = { 1, 1, 1 }, lam = 1;
  for (int it = 0; it < 24; it++) {
    float q[3];
    for (int r = 0; r < 3; r++) q[r] = M[r][0] * e[0] + M[r][1] * e[1] + M[r][2] * e[2];
    lam = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (lam < 1e-9f) break;
    for (int r = 0; r < 3; r++) e[r] = q[r] / lam;
  }
  float L = lam + EPS, step = 1.0f / L;

  float f[MAX_THR][2], z[MAX_THR][2];
  for (int k = 0; k < n; k++) {
    int i = idx[k];
    f[k][0] = a->f[i][0]; f[k][1] = a->f[i][1];
    project_piece(&v->thr[i], f[k], pc[i]);
  }
  {                                                                     /* a warm start worse than none: none */
    float r[3] = { -T[0], -T[1], -T[2] };
    for (int k = 0; k < n; k++) {
      const Thruster *t = &v->thr[idx[k]];
      r[0] += f[k][0]; r[1] += f[k][1]; r[2] += t->x * f[k][1] - t->y * f[k][0];
    }
    if (sqrtf(r[0] * r[0] + r[1] * r[1] + (wm * r[2]) * (wm * r[2])) > tn)
      for (int k = 0; k < n; k++) f[k][0] = f[k][1] = 0;
  }
  for (int k = 0; k < n; k++) { z[k][0] = f[k][0]; z[k][1] = f[k][1]; }
  float tk = 1, rel = 1, last_rel = 1e9f, rel32 = 1e9f;
  int it;
  for (it = 0; it < maxit; it++) {
    float r[3] = { -T[0], -T[1], -T[2] };
    for (int k = 0; k < n; k++) {
      const Thruster *t = &v->thr[idx[k]];
      r[0] += z[k][0]; r[1] += z[k][1]; r[2] += t->x * z[k][1] - t->y * z[k][0];
    }
    if ((it & 7) == 0) {                                                /* how near: the residual at z */
      float rr = sqrtf(r[0] * r[0] + r[1] * r[1] + (wm * r[2]) * (wm * r[2]));
      rel = tn > 1e-3f ? rr / tn : rr;
      if (rel < TOL * 0.3f) break;
      if ((it & 31) == 0) {                                             /* plainly not held: far, and stuck */
        if (it >= 128 && rel > 0.25f && rel > 0.97f * rel32) break;
        rel32 = rel;
      }
      if (rel > last_rel) tk = 1;                                         /* restart the momentum */
      last_rel = rel;
    }
    r[2] *= W[2] * W[2];
    float tk1 = 0.5f * (1.0f + sqrtf(1.0f + 4.0f * tk * tk)), mom = (tk - 1.0f) / tk1;
    for (int k = 0; k < n; k++) {
      const Thruster *t = &v->thr[idx[k]];
      float g0 = r[0] - t->y * r[2] + EPS * z[k][0];
      float g1 = r[1] + t->x * r[2] + EPS * z[k][1];
      float nf[2] = { z[k][0] - step * g0, z[k][1] - step * g1 };
      project_piece(t, nf, pc[idx[k]]);
      z[k][0] = nf[0] + mom * (nf[0] - f[k][0]);
      z[k][1] = nf[1] + mom * (nf[1] - f[k][1]);
      f[k][0] = nf[0]; f[k][1] = nf[1];
    }
    tk = tk1;
  }
  dp_iters += (uint32_t)it;
  /* the final residual, at f (feasible) */
  float r[3] = { -T[0], -T[1], -T[2] };
  for (int k = 0; k < n; k++) {
    const Thruster *t = &v->thr[idx[k]];
    r[0] += f[k][0]; r[1] += f[k][1]; r[2] += t->x * f[k][1] - t->y * f[k][0];
  }
  float rr = sqrtf(r[0] * r[0] + r[1] * r[1] + (wm * r[2]) * (wm * r[2]));
  a->res = tn > 1e-3f ? rr / tn : (rr > 1e-3f ? 1 : 0);
  a->iters = it;
  float pw[MAX_GRP + 1] = { 0 };
  for (int k = 0; k < n; k++) {
    int i = idx[k];
    const Thruster *t = &v->thr[i];
    a->f[i][0] = f[k][0]; a->f[i][1] = f[k][1];
    float m = sqrtf(f[k][0] * f[k][0] + f[k][1] * f[k][1]), cap = capacity(t, f[k]);
    a->use[i] = cap > 0 ? m / cap : 0;
    if (a->use[i] > 1) a->use[i] = 1;
    float kw = t->kw > 0 ? t->kw : t->kn / KN_KW[t->type];
    pw[t->grp] += kw * a->use[i] * sqrtf(a->use[i]);                   /* power goes as thrust^1.5 */
  }
  for (int i = 0; i < v->nthr; i++) if (!(mask >> i & 1)) a->use[i] = 0;
  a->power_ok = 1;
  for (int g = 1; g <= MAX_GRP; g++) if (v->gen[g] > 0 && pw[g] > 0.9f * v->gen[g]) a->power_ok = 0;
  a->ok = a->res <= TOL && a->power_ok;
  return a->ok;
}

/* the whole sets first; if not held and some sets are not convex, each combination of their convex pieces
   (at most 16), from a cold start: the best one stands */
int dp_alloc(const Vessel *v, uint32_t mask, const float tau[3], Alloc *a, int maxit) {
  signed char pc[MAX_THR];
  int nc[MAX_THR], k = 0;
  memset(pc, -1, sizeof pc);
  for (int i = 0; i < v->nthr; i++) { if (mask >> i & 1 && pieces(&v->thr[i]) == 2 && k < 4) nc[k++] = i; }
  dp_solves++;
  if (alloc_core(v, mask, tau, a, maxit, pc) || k == 0) return a->ok;
  Alloc best = *a;
  for (int c = 0; c < 1 << k; c++) {
    for (int j = 0; j < k; j++) pc[nc[j]] = (signed char)(c >> j & 1);
    Alloc t;
    memset(&t, 0, sizeof t);
    alloc_core(v, mask, tau, &t, maxit, pc);
    if (t.ok || t.res < best.res) best = t;
    if (t.ok) break;
  }
  *a = best;
  return a->ok;
}

uint32_t dp_case_mask(const Vessel *v, int c) {
  uint32_t m = 0;
  for (int i = 0; i < v->nthr; i++) if (c == 0 || v->thr[i].grp != c) m |= 1u << i;
  return m;
}

int dp_maxit = 600;

int dp_held(const Vessel *v, uint32_t mask, const Env *e, float from, Alloc *a) {
  float tau[3];
  dp_force(v, e, from, tau);
  tau[0] = -tau[0]; tau[1] = -tau[1]; tau[2] = -tau[2];             /* the thrusters push against the weather */
  return dp_alloc(v, mask, tau, a, dp_maxit);
}

int dp_number(const Vessel *v, uint32_t mask, float from, Alloc *a) {
  for (int bf = 1; bf < NBF; bf++) if (!dp_held(v, mask, &DP_TABLE[bf], from, a)) return bf - 1;
  return NBF - 1;
}

#define WIND_MAX 40.0f

float dp_limit_wind(const Vessel *v, uint32_t mask, float from, float cur, int num, Alloc *a) {
  float lo = 0, hi = WIND_MAX;
  int steps = 13;
  Env e;
  if (cur < 0 && num >= 0) {                                           /* between the steps the number gave */
    lo = DP_TABLE[num].wind;
    hi = num < NBF - 1 ? DP_TABLE[num + 1].wind : WIND_MAX;
    steps = 10;
  } else {
    e = dp_env_wind(0);
    if (cur >= 0) e.cur = cur;
    if (!dp_held(v, mask, &e, from, a)) return 0;
  }
  e = dp_env_wind(hi);
  if (cur >= 0) e.cur = cur;
  if (dp_held(v, mask, &e, from, a)) return hi;
  Alloc keep = *a;
  for (int s = 0; s < steps; s++) {
    float mid = 0.5f * (lo + hi);
    e = dp_env_wind(mid);
    if (cur >= 0) e.cur = cur;
    *a = keep;
    if (dp_held(v, mask, &e, from, a)) { lo = mid; keep = *a; } else hi = mid;
  }
  *a = keep;
  return lo;
}

static int in_bow(int h) { return h <= 3 || h >= NHEAD - 3; }          /* within 30 degrees of the bow */

void dp_analyse(const Vessel *v, Result *r, void (*progress)(int, int)) {
  memset(r, 0, sizeof *r);
  uint32_t s0 = dp_solves, i0 = dp_iters;
  r->ncase = 1 + v->ngrp;
  int total = r->ncase * NHEAD, done = 0;
  for (int c = 0; c < r->ncase; c++) {
    uint32_t m = dp_case_mask(v, c);
    Alloc a;
    memset(&a, 0, sizeof a);
    for (int h = 0; h < NHEAD; h++) {
      float from = (float)(h * 10);
      int num = dp_number(v, m, from, &a);
      r->num[c][h] = (signed char)num;
      r->wind[c][h] = dp_limit_wind(v, m, from, -1, num, &a);
      if (progress) progress(++done, total);
    }
  }
  r->A = r->B = 99;
  r->C = r->D = v->ngrp > 1 ? 99 : -1;
  for (int h = 0; h < NHEAD; h++) {
    int w = 99; float ww = 99;
    for (int c = 1; c < r->ncase; c++) {
      if (r->num[c][h] < w) w = r->num[c][h];
      if (r->wind[c][h] < ww) ww = r->wind[c][h];
    }
    r->wnum[h] = (signed char)(w == 99 ? r->num[0][h] : w);
    r->wwind[h] = ww == 99 ? r->wind[0][h] : ww;
    if (r->num[0][h] < r->B) r->B = r->num[0][h];
    if (in_bow(h) && r->num[0][h] < r->A) r->A = r->num[0][h];
    if (v->ngrp > 1) {
      if (r->wnum[h] < r->D) r->D = r->wnum[h];
      if (in_bow(h) && r->wnum[h] < r->C) r->C = r->wnum[h];
    }
  }
  r->solves = dp_solves - s0;
  r->iters = dp_iters - i0;
}

/* ------------------------------------------------------------------ the vessel file (.DPV), plain text
   key=value lines; # starts a comment.
     name=EXAMPLE DSV      lpp=110  b=22  t=7       (m)
     afw=420  alw=1500  xlw=5                        (wind areas m2, side area centre m fwd of midships)
     alc=770  xlc=0                                  (current: underwater side area, centre; default lpp*t, 0)
     gen=1,6000                                      (kW a redundancy group generates; none: no limit)
     thr=NAME,TYPE,x,y,kW,kN,group,fzFrom,fzTo,OPTS  (TYPE TUN AZI AZD PROP; kN 0: from kW;
                                                       OPTS: R rudder, C CPP, D ducted, I rounded tunnel inlet) */
static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
  return s;
}

static int split(char *s, char *out[], int max) {
  int n = 0;
  for (;;) {
    char *c = strchr(s, ',');
    if (c) *c = 0;
    if (n < max) out[n++] = trim(s);
    if (!c) break;
    s = c + 1;
  }
  return n;
}

static int ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) { char x = *a, y = *b; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return 0; }
  return *a == *b;
}

int dp_parse(Vessel *v, const char *text, char *err, int errn) {
  memset(v, 0, sizeof *v);
  v->alc = -1;
  strcpy(v->name, "vessel");
  char line[160];
  int ln = 0;
  const char *p = text;
  while (*p) {
    int k = 0;
    while (*p && *p != '\n') { if (k < (int)sizeof line - 1) line[k++] = *p; p++; }
    if (*p) p++;
    line[k] = 0;
    ln++;
    char *h = strchr(line, '#');
    if (h) *h = 0;
    char *s = trim(line);
    if (!*s) continue;
    char *eq = strchr(s, '=');
    if (!eq) { snprintf(err, (size_t)errn, "line %d: no '='", ln); return -1; }
    *eq = 0;
    char *key = trim(s), *val = trim(eq + 1);
    float f = (float)atof(val);
    if (ieq(key, "name")) { strncpy(v->name, val, sizeof v->name - 1); v->name[sizeof v->name - 1] = 0; }
    else if (ieq(key, "lpp")) v->lpp = f;
    else if (ieq(key, "b")) v->b = f;
    else if (ieq(key, "t")) v->t = f;
    else if (ieq(key, "afw")) v->afw = f;
    else if (ieq(key, "alw")) v->alw = f;
    else if (ieq(key, "xlw")) v->xlw = f;
    else if (ieq(key, "alc")) v->alc = f;
    else if (ieq(key, "xlc")) v->xlc = f;
    else if (ieq(key, "gen")) {
      char *F[2];
      if (split(val, F, 2) != 2 || atoi(F[0]) < 1 || atoi(F[0]) > MAX_GRP) {
        snprintf(err, (size_t)errn, "line %d: gen=group,kW", ln); return -1; }
      v->gen[atoi(F[0])] = (float)atof(F[1]);
    }
    else if (ieq(key, "thr")) {
      if (v->nthr >= MAX_THR) { snprintf(err, (size_t)errn, "line %d: at most %d thrusters", ln, MAX_THR); return -1; }
      char *F[10];
      int n = split(val, F, 10);
      if (n < 7) { snprintf(err, (size_t)errn, "line %d: thr=NAME,TYPE,x,y,kW,kN,group", ln); return -1; }
      Thruster *t = &v->thr[v->nthr];
      memset(t, 0, sizeof *t);
      strncpy(t->name, F[0], sizeof t->name - 1);
      t->type = -1;
      for (int i = 0; i < T_CNT; i++) if (ieq(F[1], DP_TYPE[i])) t->type = i;
      if (t->type < 0) { snprintf(err, (size_t)errn, "line %d: type %s? (TUN AZI AZD PROP)", ln, F[1]); return -1; }
      t->x = (float)atof(F[2]); t->y = (float)atof(F[3]);
      t->kw = (float)atof(F[4]); t->kn = (float)atof(F[5]);
      t->grp = atoi(F[6]);
      if (n > 8) { t->fz_a = wrap360((float)atof(F[7])); t->fz_b = wrap360((float)atof(F[8])); }
      if (n > 9) for (const char *o = F[9]; *o; o++) {
        char c = *o >= 'a' ? *o - 32 : *o;
        if (c == 'R') t->rudder = 1;
        if (c == 'C') t->cpp = 1;
        if (c == 'D') t->duct = 1;
        if (c == 'I') t->rounded = 1;
      }
      if (t->kw <= 0 && t->kn <= 0) { snprintf(err, (size_t)errn, "line %d: kW or kN needed", ln); return -1; }
      v->nthr++;
    }
    else { snprintf(err, (size_t)errn, "line %d: %s?", ln, key); return -1; }
  }
  if (v->lpp <= 0 || v->b <= 0 || v->t <= 0) { snprintf(err, (size_t)errn, "lpp, b and t are needed"); return -1; }
  if (v->nthr == 0) { snprintf(err, (size_t)errn, "no thrusters"); return -1; }
  if (v->alc < 0) v->alc = v->lpp * v->t;
  dp_prepare(v);
  return 0;
}

/* a made-up DSV, about 120 m: not any real vessel */
const char DP_EXAMPLE[] =
  "# DPCAP47 vessel file. A made-up DSV of about 120 m: not any real vessel.\n"
  "name=EXAMPLE DSV\n"
  "lpp=110\nb=22\nt=7\n"
  "afw=420\nalw=1500\nxlw=5\n"
  "alc=770\nxlc=0\n"
  "# thr=NAME,TYPE,x,y,kW,kN,group,fzFrom,fzTo,OPTS\n"
  "thr=BT1,TUN,48,0,1500,0,1\n"
  "thr=BT2,TUN,45,0,1500,0,2\n"
  "thr=BAZ,AZD,40,0,1500,0,3,80,100\n"
  "thr=AZP,AZD,-50,-6,3000,0,1\n"
  "thr=AZS,AZD,-50,6,3000,0,2\n"
  "thr=ST,TUN,-42,0,1000,0,3\n";

void dp_example(Vessel *v) {
  char e[64];
  dp_parse(v, DP_EXAMPLE, e, sizeof e);
}

/* the map: the limiting wind for each heading and each current from 0 to 2 m/s (NCUR steps), the rest of the
   weather as Table 2-1 has it for the wind. map[h * NCUR + c], m/s. */
void dp_map(const Vessel *v, uint32_t mask, float *map, void (*progress)(int, int)) {
  Alloc a;
  memset(&a, 0, sizeof a);
  int done = 0;
  for (int h = 0; h < NHEAD; h++)
    for (int c = 0; c < NCUR; c++) {
      map[h * NCUR + c] = dp_limit_wind(v, mask, (float)(h * 10), (float)c * (2.0f / (NCUR - 1)), -1, &a);
      if (progress) progress(++done, NHEAD * NCUR);
    }
}
