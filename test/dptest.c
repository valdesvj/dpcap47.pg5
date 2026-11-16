/* dptest.c - DPCAP47's engine on the PC (make test). Each check prints ok or FAIL; exit 1 on a FAIL.
 * Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
 *   dptest            the checks
 *   dptest FILE.DPV   the level 1 analysis of a vessel file, as a table (and the time it took on this PC) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "dpcap.h"

static int fails;
#define CHECK(c, ...) do { printf((c) ? "ok   " : "FAIL "); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static float frand(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }

static void print_result(const Vessel *v, const Result *r) {
  printf("%s: %d thrusters, %d groups\n", v->name, v->nthr, v->ngrp);
  for (int i = 0; i < v->nthr; i++) {
    const Thruster *t = &v->thr[i];
    printf("  %-6s %-4s x %6.1f y %5.1f  grp %d  %6.1f kN ahead %6.1f astern\n", t->name, DP_TYPE[t->type], t->x, t->y,
           t->grp, t->tf, t->tr);
  }
  printf("from  intact        ");
  for (int c = 1; c < r->ncase; c++) printf("grp%d lost      ", c);
  printf("WCSF\n");
  for (int h = 0; h < NHEAD; h++) {
    printf("%3d  ", h * 10);
    for (int c = 0; c < r->ncase; c++) printf("%2d %5.1f m/s   ", r->num[c][h], r->wind[c][h]);
    printf("%2d %5.1f\n", r->wnum[h], r->wwind[h]);
  }
  char s[4][4];
  int n[4] = { r->A, r->B, r->C, r->D };
  for (int i = 0; i < 4; i++) { if (n[i] < 0) strcpy(s[i], "NA"); else sprintf(s[i], "%d", n[i]); }
  printf("DP Capability-L1 (%s, %s, %s, %s)   %u allocations, %u iterations\n", s[0], s[1], s[2], s[3],
         (unsigned)r->solves, (unsigned)r->iters);
}

/* a random point of a piece of a thruster's set */
static void piece_point(const Piece *p, float f[2]) {
  float a, r;
  switch (p->kind) {
  case K_SEG: r = frand(0, 1); f[0] = p->v[0][0] + r * (p->v[1][0] - p->v[0][0]); f[1] = p->v[0][1] + r * (p->v[1][1] - p->v[0][1]); return;
  case K_DISK: a = frand(0, 360); r = frand(0, p->r); break;
  case K_SECT: a = p->a0 + frand(0, p->span); r = frand(0, p->r); break;
  default: {                                       /* a triangle of the fan from corner 0, a random point in it */
    int k = 1 + rand() % (p->nv - 2);
    float u = frand(0, 1), w = frand(0, 1);
    if (u + w > 1) { u = 1 - u; w = 1 - w; }
    f[0] = p->v[0][0] + u * (p->v[k][0] - p->v[0][0]) + w * (p->v[k + 1][0] - p->v[0][0]);
    f[1] = p->v[0][1] + u * (p->v[k][1] - p->v[0][1]) + w * (p->v[k + 1][1] - p->v[0][1]);
    return;
  }
  }
  f[0] = r * cosf(a * 3.14159265f / 180); f[1] = r * sinf(a * 3.14159265f / 180);
}

/* a force the thrusters can surely give: each at a random point of its own set, scaled by k */
static void reachable(const Vessel *v, uint32_t m, float k, float tau[3]) {
  const Region *rg = dp_regions(v, m);
  tau[0] = tau[1] = tau[2] = 0;
  for (int i = 0; i < v->nthr; i++) {
    if (!(m >> i & 1) || rg[i].n == 0) continue;
    const Thruster *t = &v->thr[i];
    float f[2];
    piece_point(&rg[i].p[rand() % rg[i].n], f);
    tau[0] += k * f[0]; tau[1] += k * f[1]; tau[2] += k * (t->x * f[1] - t->y * f[0]);
  }
}

/* does a piece reach out in direction th (degrees), farther than r? */
static float reach(const Piece *p, float th) {
  float ux = cosf(th * 3.14159265f / 180), uy = sinf(th * 3.14159265f / 180), best = 0;
  if (p->kind == K_DISK) return p->r;
  if (p->kind == K_SECT) { float d = fmodf(th - p->a0 + 720.0f, 360.0f); return d <= p->span ? p->r : 0; }
  if (p->kind == K_SEG) {
    for (int k = 0; k < 2; k++) {
      float x = p->v[k][0], y = p->v[k][1], m = sqrtf(x * x + y * y);
      if (m > 1e-3f && x * ux + y * uy > 0.999f * m && m > best) best = m;
    }
    return best;
  }
  for (int k = 1; k < p->nv; k++) {                /* a polygon: its corners along th */
    float x = p->v[k][0], y = p->v[k][1], m = sqrtf(x * x + y * y);
    if (m > 1e-3f && x * ux + y * uy > 0.9995f * m && m > best) best = m;
  }
  return best;
}

static float reach_all(const Region *r, float th) {
  float b = 0;
  for (int k = 0; k < r->n; k++) { float x = reach(&r->p[k], th); if (x > b) b = x; }
  return b;
}

static const char SYM[] =
  "name=SYMMETRIC\nlpp=100\nb=20\nt=6\nafw=350\nalw=1200\nxlw=0\nlos=104\nbowdeg=30\nawlaft=1000\n"
  "thr=B1,TUN,45,0,1200,0,1,0,0,,2.2,2.5\nthr=B2,TUN,42,0,1200,0,2,0,0,,2.2,2.5\n"
  "thr=AP,AZD,-45,-5,2500,0,1,0,0,,3.2,1.5\nthr=AS,AZD,-45,5,2500,0,2,0,0,,3.2,1.5\n";

static const char PROPS[] =
  "name=PROPS\nlpp=90\nb=18\nt=6\nafw=300\nalw=900\nxlw=3\n"
  "thr=BT,TUN,40,0,800,0,1,0,0,,1.8,2.5\nthr=MP,PROP,-42,-4,2000,0,1,0,0,RC,3.4,2.4,5.5\n"
  "thr=MS,PROP,-42,4,2000,0,2,0,0,RC,3.4,2.4,5.5\nthr=ST,TUN,-35,0,600,0,2,0,0,,1.6,2.5\n";

static int load(const char *path, Vessel *v) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return -1; }
  static char buf[16384];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  char e[80];
  if (dp_parse(v, buf, e, sizeof e)) { printf("%s: %s\n", path, e); return -1; }
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 5) {                     /* FILE CASE FROM BF: the force to give and the thrusters (tools/lpcheck.py) */
    Vessel v;
    if (load(argv[1], &v)) return 1;
    float tau[3];
    dp_force(&v, &DP_TABLE[atoi(argv[4])], (float)atof(argv[3]), tau);
    printf("TAU %g %g %g\n", -tau[0], -tau[1], -tau[2]);
    uint32_t m = dp_case_mask(&v, atoi(argv[2]));
    Env e = DP_TABLE[atoi(argv[4])];
    float beta[MAX_THR];
    dp_vent(&v, &e, (float)atof(argv[3]), beta);
    const Region *rg = dp_regions(&v, m);
    for (int i = 0; i < v.nthr; i++) {                 /* each working thruster's set: its convex pieces */
      const Thruster *t = &v.thr[i];
      if (!(m >> i & 1)) continue;
      printf("T %d %g %g %g\n", i, t->x, t->y, beta[i]);
      for (int k = 0; k < rg[i].n; k++) {
        const Piece *p = &rg[i].p[k];
        printf("P %d %g %g %g", p->kind, p->a0, p->span, p->r);
        for (int q = 0; q < (p->kind == K_SEG ? 2 : p->kind == K_POLY ? p->nv : 0); q++) printf(" %g %g", p->v[q][0], p->v[q][1]);
        printf("\n");
      }
    }
    Alloc a; memset(&a, 0, sizeof a);
    printf("HELD %d\n", dp_held(&v, m, &e, (float)atof(argv[3]), &a));
    return 0;
  }
  if (argc > 1) {
    Vessel v;
    if (load(argv[1], &v)) return 1;
    Result r;
    double t0 = now();
    dp_analyse(&v, &r, NULL);
    print_result(&v, &r);
    printf("%.3f s on this PC\n", now() - t0);
    return 0;
  }

  /* 1. Table 2-1, and the environment between its steps */
  int good = 1;
  for (int b = 0; b < NBF; b++) {
    Env e = dp_env_wind(DP_TABLE[b].wind);
    if (fabsf(e.hs - DP_TABLE[b].hs) > 1e-4f || fabsf(e.cur - DP_TABLE[b].cur) > 1e-4f) good = 0;
  }
  Env m = dp_env_wind(15.45f);
  CHECK(good && fabsf(m.hs - 3.65f) < 1e-3f && DP_TABLE[8].wind == 20.7f && DP_TABLE[8].hs == 5.7f,
        "Table 2-1 at its steps and between them (15.45 m/s: Hs %.2f m)", m.hs);

  /* 2. the example vessel file, and the efficiencies */
  Vessel v;
  char err[80];
  int pe = dp_parse(&v, DP_EXAMPLE, err, sizeof err);
  CHECK(pe == 0 && v.nthr == 6 && v.ngrp == 3, "the example vessel file reads (%s)", pe ? err : v.name);
  double bt = 900 * 0.93 * pow(2.4 * 1500 * 0.93, 2.0 / 3) / 1000 * 0.9;      /* 3.9.2: eta1 900, eta2 0.93, etaM 0.93 */
  double az = 1200 * pow(3.6 * 3000 * 0.93, 2.0 / 3) / 1000 * 0.9;             /* ducted: eta1 1200 */
  CHECK(fabs(v.thr[0].tf - bt) < 0.05 && fabs(v.thr[3].tf - az) < 0.2 && !v.thr[0].flags && !v.assumed,
        "thrust (3.9.2): tunnel %.1f kN, ducted azimuth %.1f kN (x 0.9 misc.)", v.thr[0].tf, v.thr[3].tf);
  {
    Vessel vp2; dp_parse(&vp2, PROPS, err, sizeof err);
    double cy = 0.0126 * 1.1 * 5.5 / (3.4 * 3.4), side = cy * 30;
    CHECK(fabs(vp2.thr[1].cy - cy) < 1e-5 && fabs(vp2.thr[1].tr / vp2.thr[1].tf - 0.65) < 1e-4,
          "rudder (3.10): Cy %.4f a degree, side force at 30 degrees %.0f %% of the thrust; CPP astern 0.65", cy, side * 100);
  }
  Vessel bad;
  CHECK(dp_parse(&bad, "lpp=100\nb=20\nt=5\nthr=X,FOO,0,0,100,0,1\n", err, sizeof err) && strstr(err, "FOO"),
        "a bad file says why: %s", err);

  /* 3. forces: head on pushes aft and nothing sideways; from starboard pushes to port; scale with the factor */
  float tau[3];
  dp_force(&v, &DP_TABLE[6], 0, tau);
  float ty = tau[1];
  dp_force(&v, &DP_TABLE[6], 90, tau);
  CHECK(tau[1] < 0 && fabsf(ty) < 1e-3f, "forces: head on F %.0f kN sideways, from starboard %.0f kN (to port)", ty, tau[1]);
  dp_dyn = 1.0f; float t1[3]; dp_force(&v, &DP_TABLE[6], 90, t1); dp_dyn = 1.25f;
  CHECK(fabsf(tau[1] / t1[1] - 1.25f) < 1e-4f, "the dynamic factor 1.25");
  {                                     /* the formulas of 3.5 - 3.7, worked out here again in double */
    float o[9];
    dp_force_parts(&v, &DP_TABLE[6], 90, o);
    double fyw = -0.5 * 1.226 * 13.8 * 13.8 * 1500 * 0.9 / 1000, fyc = -0.5 * 1026 * 0.75 * 0.75 * 770 * 0.6 / 1000;
    int ok = fabs(o[1] - fyw) < 0.01 && fabs(o[2] - fyw * 5) < 0.05 && fabs(o[4] - fyc) < 0.01 && fabs(o[5]) < 0.01;
    dp_force_parts(&v, &DP_TABLE[8], 0, o);                     /* head seas, Beaufort 8 */
    double bow = 34 * M_PI / 180, cw = 1150 / (55.0 * 22), h1 = 0.8 * pow(bow, 0.45);
    double h2 = 0.05 + 0.95 * atan(1.45 * (0 - 1.75)), tz = 10.0 / 1.4049;
    double tsu = tz / (0.9 * pow(110, 0.33)), f = tsu < 1 ? 1 : pow(tsu, -3) * exp(1 - pow(tsu, -3));
    double fxd = 0.5 * 1026 * 9.81 * 5.7 * 5.7 * 22 * 0.09 * h1 * h2 * f / 1000;
    double fxw = 0.5 * 1.226 * 20.7 * 20.7 * 420 * -0.7 / 1000, fxc = 0.5 * 1026 * 0.5625 * 22 * 7 * -0.07 / 1000;
    (void)cw;
    ok = ok && fabs(o[6] - fxd) < 0.05 && fabs(o[0] - fxw) < 0.01 && fabs(o[3] - fxc) < 0.01 && fabs(o[7]) < 1e-3;
    CHECK(ok, "3.5 - 3.7: beam wind %.0f kN and current %.0f kN at Beaufort 6; head seas Beaufort 8: wind %.0f, "
          "current %.1f, wave drift %.0f kN", fyw, fyc, fxw, fxc, fxd);
    dp_force_parts(&v, &DP_TABLE[8], 180, o);
    CHECK(o[6] > 0 && o[0] > 0, "seas and wind from astern push ahead (drift %+.0f kN)", o[6]);
  }
  {                                     /* 3.9.4 ventilation: 0 < beta <= 1, less in a big sea */
    float b1[MAX_THR], b11[MAX_THR];
    dp_vent(&v, &DP_TABLE[1], 0, b1);
    dp_vent(&v, &DP_TABLE[11], 0, b11);
    int ok = 1;
    for (int i = 0; i < v.nthr; i++) if (!(b11[i] > 0 && b11[i] <= b1[i] + 1e-6f && b1[i] <= 1)) ok = 0;
    CHECK(ok, "ventilation (3.9.4): Beaufort 1 / 11: tunnel %.3f / %.3f, aft azimuth %.3f / %.3f", b1[0], b11[0], b1[3], b11[3]);
  }
  {                                     /* 3.11.3 - 3.11.5: flushing */
    const Region *rg = dp_regions(&v, dp_case_mask(&v, 0));
    float h = atanf(0.1f + 3.6f / 12) * 180 / 3.14159265f;      /* AZP must not wash AZS (it pushes to port, 270) */
    int ok = reach_all(&rg[3], 270) < 1 && reach_all(&rg[3], 270 + h - 2) < 1 && reach_all(&rg[3], 270 + h + 3) > 1 &&
             reach_all(&rg[4], 90) < 1;
    int poly = 0;
    for (int k = 0; k < rg[3].n; k++) if (rg[3].p[k].kind == K_POLY) poly = 1;   /* the skeg's notch */
    CHECK(ok && poly, "3.11.3: the aft azimuths may not wash each other (sector +-%.1f deg); 3.11.5: AZP loses near the skeg", h);
    rg = dp_regions(&v, dp_case_mask(&v, 1));                    /* group 1 lost: AZP dead, AZS may wash it */
    float sd = 12 / 3.6f, b = 1 - 1 / (0.02f * sd * sd + 0.25f * sd + 1.2f), got = reach_all(&rg[4], 90) / v.thr[4].tf;
    CHECK(fabsf(got - b) < 0.01f, "3.11.4: AZS washing the dead AZP keeps %.3f of its thrust (formula %.3f)", got, b);
  }

  /* 3b. the thrusters push against the weather: from starboard they push to starboard, head on ahead */
  {
    Alloc h; memset(&h, 0, sizeof h);
    dp_held(&v, dp_case_mask(&v, 0), &DP_TABLE[5], 90, &h);
    float sy = 0; for (int i = 0; i < v.nthr; i++) sy += h.f[i][1];
    memset(&h, 0, sizeof h);
    dp_held(&v, dp_case_mask(&v, 0), &DP_TABLE[5], 0, &h);
    float sx = 0; for (int i = 0; i < v.nthr; i++) sx += h.f[i][0];
    CHECK(h.ok && sy > 0 && sx > 0, "the thrusters push against the weather (from starboard %+.0f kN y, head on %+.0f kN x)", sy, sx);
  }

  /* 4. the allocation reaches any force the thrusters can give (90 % of a random reachable one) */
  srand(47);
  int reached = 0, tries = 300, fz_ok = 1;
  float worst = 0;
  for (int k = 0; k < tries; k++) {
    uint32_t mk = dp_case_mask(&v, k % 4);
    reachable(&v, mk, 0.9f, tau);
    Alloc a; memset(&a, 0, sizeof a);
    if (dp_alloc(&v, mk, tau, &a, 600)) reached++;
    if (a.res > worst) worst = a.res;
    float th = atan2f(a.f[2][1], a.f[2][0]) * 180 / 3.14159265f;          /* BAZ: 80..100 forbidden */
    if ((a.f[2][0] * a.f[2][0] + a.f[2][1] * a.f[2][1]) > 1 && th > 80.5f && th < 99.5f) fz_ok = 0;
  }
  CHECK(reached == tries, "allocation: %d of %d reachable forces held (worst residual %.2f %%)", reached, tries, worst * 100);
  CHECK(fz_ok, "the bow azimuth never pushes inside its forbidden zone 80..100");
  Vessel vp;
  dp_parse(&vp, PROPS, err, sizeof err);
  reached = 0;
  for (int k = 0; k < tries; k++) {
    reachable(&vp, dp_case_mask(&vp, 0), 0.9f, tau);
    Alloc a; memset(&a, 0, sizeof a);
    if (dp_alloc(&vp, dp_case_mask(&vp, 0), tau, &a, 600)) reached++;
  }
  CHECK(reached >= tries * 97 / 100, "propellers with rudders: %d of %d held", reached, tries);

  /* 5. too much is not held: twice the full sideways thrust */
  float side = 0;
  for (int i = 0; i < v.nthr; i++) side += v.thr[i].tf;
  float big[3] = { 0, 2 * side, 0 };
  Alloc a; memset(&a, 0, sizeof a);
  CHECK(!dp_alloc(&v, dp_case_mask(&v, 0), big, &a, 600) && a.res > 0.3f, "twice the thrust there is: not held (%.0f %% short)",
        a.res * 100);

  /* 6. a power limit makes it weaker */
  Result r0, r1;
  dp_analyse(&v, &r0, NULL);
  Vessel vg = v;
  vg.gen[1] = vg.gen[2] = vg.gen[3] = 3500;
  dp_analyse(&vg, &r1, NULL);
  int weaker = 1, some = 0;
  for (int h = 0; h < NHEAD; h++) { if (r1.wind[0][h] > r0.wind[0][h] + 0.05f) weaker = 0; if (r1.wind[0][h] < r0.wind[0][h] - 0.5f) some = 1; }
  CHECK(weaker && some, "3500 kW a group: the envelope shrinks");

  /* 7. a symmetric vessel: port and starboard the same */
  Vessel vs;
  dp_parse(&vs, SYM, err, sizeof err);
  Result rs;
  dp_analyse(&vs, &rs, NULL);
  int sym = 1;
  float wd = 0;
  for (int h = 1; h < NHEAD / 2; h++) {
    if (rs.num[0][h] != rs.num[0][NHEAD - h]) sym = 0;
    float d = fabsf(rs.wind[0][h] - rs.wind[0][NHEAD - h]);
    if (d > wd) wd = d;
  }
  CHECK(sym && wd < 0.3f, "symmetric vessel: the same numbers port and starboard (wind within %.2f m/s)", wd);
  CHECK(rs.wnum[9] <= rs.num[0][9] && rs.wnum[9] < rs.num[0][9] + 1, "a failure never helps (beam: %d intact, %d WCSF)",
        rs.num[0][9], rs.wnum[9]);

  /* 8. more iterations do not change the answer much: the envelope is converged */
  int keep = dp_maxit;
  dp_maxit = 6000;
  Result rh;
  dp_analyse(&v, &rh, NULL);
  dp_maxit = keep;
  float dw = 0; int dn = 0;
  for (int c = 0; c < r0.ncase; c++) for (int h = 0; h < NHEAD; h++) {
    float d = fabsf(rh.wind[c][h] - r0.wind[c][h]);
    if (d > dw) dw = d;
    if (rh.num[c][h] != r0.num[c][h]) dn++;
  }
  CHECK(dw < 0.6f && dn <= 2, "converged: 6000 iterations move the wind limits at most %.2f m/s, %d numbers", dw, dn);

  /* 9. A B C D */
  CHECK(r0.A >= r0.B && r0.C >= r0.D && r0.B >= r0.D && r0.A >= r0.C && r0.C >= 0, "A >= B, C >= D, intact >= WCSF");

  /* 10. the map: more current, less wind; at 0.75 m/s it agrees with the envelope */
  static float map[NHEAD * NCUR];
  double tm = now();
  dp_map(&v, dp_case_mask(&v, 0), map, NULL);
  tm = now() - tm;
  int mono = 1; float agree = 0;
  for (int h = 0; h < NHEAD; h++) {
    for (int c = 1; c < NCUR; c++) if (map[h * NCUR + c] > map[h * NCUR + c - 1] + 0.15f) mono = 0;
    float d = fabsf(map[h * NCUR + 6] - r0.wind[0][h]);              /* 6 x 0.125 = 0.75 m/s */
    if (r0.wind[0][h] > DP_TABLE[3].wind && d > agree) agree = d;
  }
  CHECK(mono && agree < 0.3f, "the map: more current never more wind; at 0.75 m/s within %.2f m/s of the envelope (%.3f s)",
        agree, tm);

  double t0 = now();
  dp_analyse(&v, &r0, NULL);
  double dt = now() - t0;
  print_result(&v, &r0);
  printf("the analysis: %.3f s on this PC\n", dt);
  printf(fails ? "%d FAILED\n" : "all ok\n", fails);
  return fails != 0;
}
