/* dpcap.h - DPCAP47: DP station-keeping capability, after DNV-ST-0111 level 1 (the frame of it: Table 2-1, the
 * dynamic factor, the thruster efficiency tables, the A B C D numbers). Plain C, no DMCP: the same file runs on the
 * DM42n and in the PC tests. Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
 *
 * Axes: x forward from midships, y to starboard, moments positive bow to starboard (x Fy - y Fx). Forces in kN,
 * moments in kNm, lengths in m. A direction "from" is where the weather comes from, relative to the bow, clockwise:
 * 0 head on, 90 from starboard. A thrust direction is where the thrust pushes the vessel: 0 ahead, 90 to starboard. */
#ifndef DPCAP_H
#define DPCAP_H
#include <stdint.h>

#define MAX_THR  12
#define MAX_GRP  6                      /* redundancy groups 1..MAX_GRP */
#define NHEAD    36                     /* 10 degrees */
#define NBF      12                     /* Beaufort 0..11 */
#define NCUR     17                     /* the map: current 0 .. 2 m/s in 0.125 */
#define MAX_CASE (MAX_GRP + 1)          /* 0 intact, g: group g lost */

enum { T_TUN, T_AZI, T_AZD, T_PROP, T_CNT };   /* tunnel, azimuth open, azimuth ducted, main propeller */

typedef struct {
  char  name[8];
  int   type;
  float x, y;                           /* m */
  float kw;                             /* motor power, kW */
  float kn;                             /* nominal bollard thrust, kN (0: from kw) */
  int   grp;                            /* redundancy group 1.. */
  float fz_a, fz_b;                     /* forbidden thrust directions fz_a .. fz_b (clockwise), equal: none */
  int   rudder, cpp, duct, rounded;     /* PROP: a rudder behind it, CPP, ducted; TUN: rounded inlet */
  float d;                              /* propeller diameter, m (0: not given; the PC's ABS method uses it) */
  /* derived by dp_prepare */
  float tf, tr;                         /* kN available ahead (or both ways for a tunnel), astern */
  float cl;                             /* PROP with rudder: side force / thrust at 30 degrees */
} Thruster;

typedef struct {
  char  name[32];
  float lpp, b, t;                      /* m */
  float afw, alw, xlw;                  /* wind: frontal and side area (m2), side area centre (m fwd of midships) */
  float alc, xlc;                       /* current: underwater side area (m2), its centre */
  int   nthr;
  Thruster thr[MAX_THR];
  int   ngrp;                           /* highest group number used */
  float gen[MAX_GRP + 1];               /* kW generated a group (0: no limit) */
} Vessel;

typedef struct { float wind, hs, tp, cur; } Env;

typedef struct {
  float f[MAX_THR][2];                  /* the allocation, kN */
  float res;                            /* residual / demand: 0 = held */
  float use[MAX_THR];                   /* |f| / capacity */
  int   ok;                             /* held (residual and power) */
  int   power_ok;
  int   iters;
} Alloc;

typedef struct {
  int   ncase;                          /* 1 + groups */
  signed char num[MAX_CASE][NHEAD];     /* DP capability number a heading (0..11) */
  float wind[MAX_CASE][NHEAD];          /* limiting wind, m/s (40 = more than we look at) */
  signed char wnum[NHEAD];              /* the worst single failure a heading */
  float wwind[NHEAD];
  int   A, B, C, D;                     /* -1: NA */
  uint32_t solves, iters;
} Result;

extern const Env DP_TABLE[NBF];         /* DNV-ST-0111 Table 2-1 */
extern const char *const DP_TYPE[T_CNT];
extern const char DP_EXAMPLE[];       /* the example vessel file */
extern float dp_dyn;                    /* dynamic factor, 1.25 at level 1 */
extern int dp_maxit;                    /* iterations an allocation may take (600) */
extern uint32_t dp_solves, dp_iters;    /* counters: allocations and their iterations */

int   dp_parse(Vessel *v, const char *text, char *err, int errn);   /* 0 ok */
void  dp_example(Vessel *v);
void  dp_prepare(Vessel *v);           /* thrust available from the efficiency tables */
Env   dp_env_wind(float wind);         /* Table 2-1 between its steps (Hs, Tp, current by the wind) */
void  dp_force(const Vessel *v, const Env *e, float from_deg, float tau[3]);   /* x the dynamic factor */
/* allocate tau, the force the thrusters must give: mask = the thrusters that work (bit i). a: warm start in, result out (may be NULL-free: zeroed) */
int   dp_alloc(const Vessel *v, uint32_t mask, const float tau[3], Alloc *a, int maxit);
uint32_t dp_case_mask(const Vessel *v, int c);   /* c 0 intact, g: group g lost */
int   dp_held(const Vessel *v, uint32_t mask, const Env *e, float from_deg, Alloc *a);
int   dp_number(const Vessel *v, uint32_t mask, float from_deg, Alloc *a);
float dp_limit_wind(const Vessel *v, uint32_t mask, float from_deg, float cur_override, int num, Alloc *a);
/* the whole level 1 analysis; progress(done, total) may be NULL */
void  dp_analyse(const Vessel *v, Result *r, void (*progress)(int, int));
/* the sizes of the structures, so a program on the PC (ctypes) can check its copy of them */
int   dp_sizeof(int what);             /* 0 Thruster, 1 Vessel, 2 Alloc, 3 Result, 4 Env */
/* the map: limiting wind a heading (36) and current (0..2 m/s, NCUR), map[h * NCUR + c] */
void  dp_map(const Vessel *v, uint32_t mask, float *map, void (*progress)(int, int));
#endif
