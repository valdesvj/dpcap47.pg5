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

/* a force the thrusters can surely give: each at a random point of its own set, scaled by k */
static void reachable(const Vessel *v, uint32_t m, float k, float tau[3]) {
  tau[0] = tau[1] = tau[2] = 0;
  for (int i = 0; i < v->nthr; i++) {
    if (!(m >> i & 1)) continue;
    const Thruster *t = &v->thr[i];
    float f[2];
    if (t->type == T_TUN) { f[0] = 0; f[1] = frand(-t->tf, t->tf); }
    else if (t->type == T_PROP) { f[0] = frand(0, t->tf); f[1] = frand(-1, 1) * t->cl * f[0]; }
    else {
      float span = fmodf(t->fz_b - t->fz_a + 360.0f, 360.0f), a;
      a = span > 0 ? t->fz_b + frand(0, 360.0f - span) : frand(0, 360);
      float r = frand(0, t->tf);
      f[0] = r * cosf(a * 3.14159265f / 180); f[1] = r * sinf(a * 3.14159265f / 180);
    }
    tau[0] += k * f[0]; tau[1] += k * f[1]; tau[2] += k * (t->x * f[1] - t->y * f[0]);
  }
}

static const char SYM[] =
  "name=SYMMETRIC\nlpp=100\nb=20\nt=6\nafw=350\nalw=1200\nxlw=0\n"
  "thr=B1,TUN,45,0,1200,0,1\nthr=B2,TUN,42,0,1200,0,2\n"
  "thr=AP,AZD,-45,-5,2500,0,1\nthr=AS,AZD,-45,5,2500,0,2\n";

static const char PROPS[] =
  "name=PROPS\nlpp=90\nb=18\nt=6\nafw=300\nalw=900\nxlw=3\n"
  "thr=BT,TUN,40,0,800,0,1\nthr=MP,PROP,-42,-4,2000,0,1,0,0,RC\nthr=MS,PROP,-42,4,2000,0,2,0,0,RC\n"
  "thr=ST,TUN,-35,0,600,0,2\n";

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
    for (int i = 0; i < v.nthr; i++) {
      const Thruster *t = &v.thr[i];
      if (m >> i & 1) printf("T %d %g %g %g %g %g %g %g\n", t->type, t->x, t->y, t->tf, t->tr, t->cl, t->fz_a, t->fz_b);
    }
    Alloc a; memset(&a, 0, sizeof a);
    Env e = DP_TABLE[atoi(argv[4])];
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
  float bt = 0.110f * 1500 * 0.93f * 0.93f * 0.9f;
  CHECK(fabsf(v.thr[0].tf - bt) < 0.01f && fabsf(v.thr[3].tf - 0.16f * 3000 * 0.93f * 0.9f) < 0.01f,
        "thrust: tunnel %.1f kN (0.93 inlet, 0.93 mech., 0.9 misc.), azimuth %.1f kN", v.thr[0].tf, v.thr[3].tf);
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
