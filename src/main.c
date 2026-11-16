/*
 * DPCAP47 for the SwissMicros DM42n (DMCP5 native program, .pg5): DP station-keeping capability, after
 * DNV-ST-0111 level 1, computed on the calculator.
 * (C) 2026 Victor Valdes (valdes.vj@gmail.com) - GPL-3.0-or-later
 *
 * The pages, on the soft keys as in Chess 47 and NAV47:
 *   PLOT   the capability plot: the limiting wind all round, intact and worst single failure, and the
 *          DP Capability-L1 (A, B, C, D) numbers. Computed when the page is first opened (and timed).
 *   TABLE  the numbers and limiting winds heading by heading, each group's failure.
 *   LIVE   the weather on the vessel now: turn its direction, raise the wind, switch thrusters off, and watch the
 *          allocation, each thruster's use and whether she holds. ENTER finds the limit for this direction.
 *   MAP    the limiting wind for every direction and every current from 0 to 2 m/s: 612 searches.
 *   VESL   the vessel file: the hull, the areas, the thrusters and the thrust each has after the losses.
 *   PRINT  the plot and the numbers to an HP 82240 printer over the infrared port.
 * PLOT and TABLE: the . key switches the method, DNV-ST-0111 level 1 or ABS SKP (the DPS Guide's station keeping
 * performance: a fixed current, the North Sea wind-wave table, SKP(a, b, c, d)).
 * EXIT: the program menu (another vessel file, setup, the method, ABS current and margin, about, off).
 * The engine is dpcap.c.
 */
#include <main.h>
#include <dmcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dpcap.h"

#define DIR      "/DPCAP47"
#define CFG_FILE DIR "/DPCAP47.CFG"
#define EX_FILE  DIR "/EXAMPLE.DPV"
#define TOP      24                       /* the page below the status bar */
#define BOT      (LCD_Y - LCD_MENU_LINES) /* and above the soft keys */
#define PI_F     3.14159265f
/* lcd_fill_rect's last argument is the byte the LCD's memory is filled with, not a colour: 1 would set one pixel
   column in eight (lines one pixel wide all but vanish). 0xFF is black, 0 is white. */
#define INK      0xFF

enum { P_PLOT, P_TABLE, P_LIVE, P_MAP, P_VESSEL, P_CNT };

static Vessel vessel;
static Result *res;                       /* heap: DMCP has 8 KB of static RAM */
static float *map;                        /* NHEAD * NCUR, heap */
static int have_res, have_map;
static uint32_t res_ms, map_ms, map_solves;
static int res_mhz;
static AbsResult *abs_res;                /* heap: ABS SKP */
static int stopped;                       /* a computation was stopped with EXIT */
static int have_abs, method;              /* method 0: DNV-ST-0111 level 1, 1: ABS SKP */
static int abs_cur10 = 15, abs_margin = 20;   /* ABS: current in tenths of a knot, margin % */
static uint32_t abs_ms;
static char vfile[64] = "";               /* the vessel file, "" = the example built in */
static char msg[64] = "";
static int page = P_VESSEL, table_top, vessel_top;   /* the vessel first: it needs no computing */

/* LIVE */
static int live_from = 90;                /* degrees */
static float live_wind = 15.0f;
static uint32_t live_off;                 /* thrusters switched off (bit i) */
static int live_fail;                     /* a group failed, 0 none */
static Alloc live_a;
static uint32_t live_us;

/* ------------------------------------------------------------------ drawing */
static void text(int x, int y, const char *s, int inv) {
  t20->inv = inv;
  lcd_setXY(t20, x, y);
  lcd_writeText(t20, s);
  t20->inv = 0;
}

static int tw(const char *s) { return lcd_textWidth(t20, s); }

static void textinv(int x, int y, const char *s) {        /* white on black */
  lcd_fill_rect((uint32_t)(x - 2), (uint32_t)y, (uint32_t)(tw(s) + 4), 20, INK);
  text(x, y, s, 1);
}

static void bar(const char *s) { lcd_fill_rect(0, 0, LCD_X, 22, INK); text(4, 1, s, 1); }

static void (*pset)(int x, int y);
static int knots;                         /* the plot's rings: 0 every 10 m/s, 1 every 10 knots (to 60) */
static int clip_y = BOT, plot_full;        /* the full-screen plot draws over the soft keys' rows */
static void px_lcd(int x, int y) { if ((unsigned)x < LCD_X && (unsigned)y < (unsigned)clip_y) lcd_fill_rect((uint32_t)x, (uint32_t)y, 1, 1, INK); }

static void line(int x0, int y0, int x1, int y1, int dash) {   /* dash: 0 solid, n: n on n off */
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy, n = 0;
  for (;;) {
    if (!dash || (n / dash) % 2 == 0) pset(x0, y0);
    n++;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

static void dots(int cx, int cy, int r) {                    /* a dotted circle */
  int n = (int)(2 * PI_F * r / 4);
  for (int i = 0; i < n; i++) {
    float a = 2 * PI_F * i / n;
    pset(cx + (int)lroundf(r * sinf(a)), cy - (int)lroundf(r * cosf(a)));
  }
}

static void circle(int cx, int cy, int r, int dash) {
  int n = r < 30 ? 24 : 72;
  for (int i = 0; i < n; i++) {
    float a0 = 2 * PI_F * i / n, a1 = 2 * PI_F * (i + 1) / n;
    if (dash && i % 2) continue;
    line(cx + (int)lroundf(r * sinf(a0)), cy - (int)lroundf(r * cosf(a0)), cx + (int)lroundf(r * sinf(a1)),
         cy - (int)lroundf(r * cosf(a1)), 0);
  }
}

static void disc(int cx, int cy, int r) {
  for (int y = -r; y <= r; y++) {
    int w = (int)sqrtf((float)(r * r - y * y));
    for (int x = -w; x <= w; x++) pset(cx + x, cy + y);
  }
}

static void arrow(int x0, int y0, int x1, int y1) {          /* from x0,y0 to a head at x1,y1 */
  line(x0, y0, x1, y1, 0);
  float a = atan2f((float)(y1 - y0), (float)(x1 - x0));
  for (int s = -1; s <= 1; s += 2) {
    float b = a + PI_F + s * 0.45f;
    line(x1, y1, x1 + (int)lroundf(7 * cosf(b)), y1 + (int)lroundf(7 * sinf(b)), 0);
  }
}

/* numbers without printf's floats: v with d decimals */
static char *fmt(char *s, float v, int d) {
  int neg = v < 0;
  if (neg) v = -v;
  int m = d == 0 ? 1 : d == 1 ? 10 : 100;
  long n = lroundf(v * m);
  if (d) sprintf(s, "%s%ld.%0*ld", neg && n ? "-" : "", n / m, d, n % m);
  else sprintf(s, "%s%ld", neg && n ? "-" : "", n);
  return s;
}

static const char *num_s(int n, char *s) { if (n < 0) strcpy(s, "NA"); else sprintf(s, "%d", n); return s; }

/* ------------------------------------------------------------------ keys */
static int read_keys(int *k1, int *k2) {
  *k1 = *k2 = 0;
  __sysfn_read_key(k1, k2);
  return (*k1 > 0) + (*k2 > 0);
}
static void wait_release(void) { int a, b; while (read_keys(&a, &b)) sys_delay(10); key_pop_all(); }

static void power_off(void);
static int wait_key(void) {
  for (;;) {
    if (key_empty()) { sys_timer_start(0, 500); CLR_ST(STAT_RUNNING); sys_sleep(); }
    if (ST(STAT_PGM_END) || ST(STAT_SUSPENDED)) { power_off(); return 0; }
    int k = key_pop();
    if (k > 0 && k != KEY_DOUBLE_RELEASE) return k;
  }
}

/* ------------------------------------------------------------------ files */
static int read_file(const char *path, char **out) {
  FIL f; UINT br;
  if (f_open(&f, path, FA_READ) != FR_OK) return -1;
  uint32_t n = (uint32_t)f_size(&f);
  if (n > 16384) { f_close(&f); return -2; }
  char *b = malloc(n + 1);
  if (!b) { f_close(&f); return -3; }
  f_read(&f, b, n, &br);
  f_close(&f);
  b[br] = 0;
  *out = b;
  return 0;
}

static void write_file(const char *path, const char *s) {
  if (!sys_disk_ok()) return;
  sys_disk_write_enable(1);
  check_create_dir(DIR);
  FIL f; UINT bw;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) { f_write(&f, s, (UINT)strlen(s), &bw); f_close(&f); }
  sys_disk_write_enable(0);
}

static void cfg_save(void) {
  char s[160];
  sprintf(s, "vessel=%s\nmethod=%d\nabscur=%d\nabsmargin=%d\nknots=%d\n", vfile, method, abs_cur10, abs_margin, knots);
  write_file(CFG_FILE, s);
}

static int vessel_load(const char *path) {      /* 0 ok, else msg says why; "" = the example */
  Vessel v;
  char err[48];
  if (!path[0]) { dp_example(&vessel); vfile[0] = 0; goto fresh; }
  char *b;
  int e = read_file(path, &b);
  if (e) { snprintf(msg, sizeof msg, e == -1 ? "cannot read %s" : "too big: %s", path); return -1; }
  e = dp_parse(&v, b, err, sizeof err);
  free(b);
  if (e) { snprintf(msg, sizeof msg, "%s", err); return -1; }
  vessel = v;
  snprintf(vfile, sizeof vfile, "%s", path);
fresh:
  have_res = have_map = have_abs = stopped = 0;
  live_off = 0; live_fail = 0;
  memset(&live_a, 0, sizeof live_a);
  return 0;
}

static void cfg_load(void) {
  char *b;
  if (read_file(CFG_FILE, &b)) return;
  char *p = strstr(b, "vessel=");
  if (p) {
    p += 7;
    char path[64];
    int n = 0;
    while (*p && *p != '\n' && *p != '\r' && n < 63) path[n++] = *p++;
    path[n] = 0;
    if (path[0] && vessel_load(path)) vessel_load("");
  }
  if ((p = strstr(b, "method="))) method = atoi(p + 7) == 1;
  if ((p = strstr(b, "knots="))) knots = atoi(p + 6) == 1;
  if ((p = strstr(b, "abscur="))) { int n = atoi(p + 7); if (n >= 0 && n <= 50) abs_cur10 = n; }
  if ((p = strstr(b, "absmargin="))) { int n = atoi(p + 10); if (n >= 0 && n <= 50) abs_margin = n; }
  free(b);
}

static int pick_cb(const char *fpath, const char *fname, void *data) {
  (void)fname; (void)data;
  return vessel_load(fpath) ? 0 : MRET_EXIT;
}

/* ------------------------------------------------------------------ computing, with a progress bar */
static uint32_t t_start;
static const char *busy_what;
static void progress(int done, int total) {
  static int last;
#ifndef DPCAP47_SIM                         /* EXIT stops the computation (the simulator's keys are a script) */
  while (!key_empty()) if (key_pop() == KEY_EXIT) dp_abort = 1;
#endif
  int w = 360 * done / total;
  if (done != total && w == last && done > 1) return;
  last = w;
  char s[48], t[16];
  lcd_fill_rect(20, 110, 360, 16, 0);
  lcd_fill_rect(20, 110, (uint32_t)w, 16, INK);
  lcd_fill_rect(20, 110, 360, 1, INK); lcd_fill_rect(20, 125, 360, 1, INK);
  lcd_fill_rect(20, 110, 1, 16, INK); lcd_fill_rect(379, 110, 1, 16, INK);
  lcd_fill_rect(0, 132, LCD_X, 44, 0);
  sprintf(s, "%s %d / %d    EXIT stops", busy_what, done, total);
  text(20, 134, s, 0);
  uint32_t el = (sys_current_ms() - t_start) / 1000, left = done ? el * (uint32_t)(total - done) / (uint32_t)done : 0;
  (void)t;
  sprintf(s, "%u s gone, about %u s left", (unsigned)el, (unsigned)left);
  text(20, 156, s, 0);
  lcd_refresh_lines(108, 70);
  reset_auto_off();
}

static void busy_screen(const char *title, const char *l1) {
  lcd_clear_buf();
  bar(title);
  text(20, 40, vessel.name, 0);
  text(20, 64, l1, 0);
  lcd_refresh();
}

static void compute(void) {
  if (!res) return;
  char s[64];
  sprintf(s, "%d directions x %d cases (intact + groups)", NHEAD, 1 + vessel.ngrp);
  busy_screen("DPCAP47: level 1 analysis", s);
  busy_what = "direction";
  dp_solves = 0;
  t_start = sys_current_ms();
  dp_abort = 0;
  dp_analyse(&vessel, res, progress);
  res_ms = sys_current_ms() - t_start;
  res_mhz = usb_powered() ? 160 : 80;
  have_res = !dp_abort; stopped = dp_abort; dp_abort = 0;
}

static void compute_abs(void) {
  if (!abs_res) return;
  char s[64], t[12];
  sprintf(s, "%d directions x %d cases, current %s kn", NHEAD, 1 + vessel.nthr + (vessel.ngrp > 1 ? vessel.ngrp : 0),
          fmt(t, abs_cur10 / 10.0f, 1));
  busy_screen("DPCAP47: ABS SKP analysis", s);
  busy_what = "direction";
  dp_solves = 0;
  t_start = sys_current_ms();
  dp_abort = 0;
  dp_abs(&vessel, abs_cur10 / 10.0f, abs_margin / 100.0f, abs_res, progress);
  abs_ms = sys_current_ms() - t_start;
  res_mhz = usb_powered() ? 160 : 80;
  have_abs = !dp_abort; stopped = dp_abort; dp_abort = 0;
}

static void compute_method(void) { if (method) compute_abs(); else compute(); }
static int have_method(void) { return method ? have_abs : have_res; }

static int need_result(void) {            /* 1: there is a result to show; else the page says why not */
  if (have_method()) return 1;             /* nothing is computed unasked: on the DM42n it takes minutes */
  char s[64];
  lcd_clear_buf();
  bar(method ? "DPCAP47  ABS SKP" : "DPCAP47  DNV-ST-0111 level 1");
  text(20, 40, vessel.name, 0);
  text(20, 64, stopped ? "The computation was stopped." : "Not computed yet.", 0);
  sprintf(s, "%d directions x %d cases", NHEAD, method ? 1 + vessel.nthr + (vessel.ngrp > 1 ? vessel.ngrp : 0) : 1 + vessel.ngrp);
  text(20, 88, s, 0);
  text(20, 112, "It may take some minutes.", 0);
  textinv(20, 140, "ENTER: compute   EXIT: stop");
  text(20, 168, ". DNV / ABS", 0);
  return 0;
}

static void compute_map(void) {
  if (!map) return;
  busy_screen("DPCAP47: the current map", "36 directions x 17 currents, intact");
  busy_what = "search";
  dp_solves = 0;
  t_start = sys_current_ms();
  dp_abort = 0;
  dp_map(&vessel, dp_case_mask(&vessel, 0), map, progress);
  map_ms = sys_current_ms() - t_start;
  map_solves = dp_solves;
  have_map = !dp_abort; dp_abort = 0;
}

/* ------------------------------------------------------------------ PLOT */
#define WSCALE 36.0f                      /* m/s at the plot's edge */

static void ship_icon(int cx, int cy, int len, float rot) {   /* a hull, bow up (rot 0) */
  float L = (float)len / 2, B = L * 0.36f;
  float P[6][2] = { { 0, -L }, { B, -L * 0.45f }, { B, L }, { -B, L }, { -B, -L * 0.45f }, { 0, -L } };
  float c = cosf(rot), s = sinf(rot);
  for (int i = 0; i < 5; i++)
    line(cx + (int)lroundf(P[i][0] * c - P[i][1] * s), cy + (int)lroundf(P[i][0] * s + P[i][1] * c),
         cx + (int)lroundf(P[i + 1][0] * c - P[i + 1][1] * s), cy + (int)lroundf(P[i + 1][0] * s + P[i + 1][1] * c), 0);
}

static void envelope(int cx, int cy, int R, const float *w, int dash, int thick) {
  int x0 = 0, y0 = 0, fx = 0, fy = 0;
  for (int h = 0; h <= NHEAD; h++) {
    float r = w[h % NHEAD] / WSCALE;
    if (r > 1) r = 1;
    float a = (float)(h % NHEAD) * 10.0f * PI_F / 180.0f;
    int x = cx + (int)lroundf(R * r * sinf(a)), y = cy - (int)lroundf(R * r * cosf(a));
    if (h) {
      line(x0, y0, x, y, dash);
      if (thick) line(x0 + 1, y0, x + 1, y, dash);
    } else { fx = x; fy = y; }
    x0 = x; y0 = y;
  }
  (void)fx; (void)fy;
}

static void plot_figure(int cx, int cy, int R, int labels) {
  float ring = knots ? 10 * 0.514444f : 10.0f;            /* the edge: 36 m/s = 70 knots */
  int nring = knots ? 6 : 3;
  for (int k = 1; k <= nring; k++) dots(cx, cy, (int)(R * k * ring / WSCALE));
  circle(cx, cy, R, 0);
  for (int d = 0; d < 360; d += 30) {
    float a = d * PI_F / 180.0f;
    line(cx + (int)lroundf((R - 5) * sinf(a)), cy - (int)lroundf((R - 5) * cosf(a)), cx + (int)lroundf(R * sinf(a)),
         cy - (int)lroundf(R * cosf(a)), 0);
  }
  ship_icon(cx, cy, R / 4, 0);
  if (method && have_abs) {
    envelope(cx, cy, R, abs_res->intact, 0, 1);
    if (abs_res->d >= 0) envelope(cx, cy, R, abs_res->group[abs_res->d_grp], 3, 0);
  } else if (!method && have_res) {
    envelope(cx, cy, R, res->wind[0], 0, 1);
    if (res->ncase > 1) envelope(cx, cy, R, res->wwind, 3, 0);
  }
  if (labels) {
    char s[8];
    for (int k = 1; k <= nring; k++) {                      /* down the 225 degree line, where it is quiet */
      if (knots && labels < 2 && k % 2) continue;           /* the small plot: 20, 40, 60 knots */
      int r = (int)(R * k * ring / WSCALE);
      sprintf(s, "%d", k * 10);
      text(cx - (int)(r * 0.7071f) - 18, cy + (int)(r * 0.7071f) - 8, s, 0);
    }
  }
}

static void skp_s(char *s, float p) { if (p < 0) strcpy(s, "-"); else sprintf(s, "%d", (int)lroundf(p)); }

static void abs_panel(int x, int y) {
  char s[64], a[8], b[8], c[8], d[8], t[16];
  const AbsResult *r = abs_res;
  text(x, y, "ABS SKP  1-min wind", 0); y += 21;
  skp_s(a, r->a); skp_s(b, r->b); skp_s(c, r->c); skp_s(d, r->d);
  sprintf(s, "(%s, %s, %s, %s)", a, b, c, d);
  textinv(x + 8, y, s); y += 24;
  sprintf(s, "a intact          %s", a); text(x, y, s, 0); y += 19;
  sprintf(s, "b least  %-6s  %s", vessel.thr[r->b_thr].name, b); text(x, y, s, 0); y += 19;
  sprintf(s, "c most   %-6s  %s", vessel.thr[r->c_thr].name, c); text(x, y, s, 0); y += 19;
  if (r->d >= 0) sprintf(s, "d WCF grp %d      %s", r->d_grp, d); else strcpy(s, "d WCF  -");
  text(x, y, s, 0); y += 19;
  sprintf(s, "e %s kn  margin %d%%", fmt(t, r->cur_kn, 1), (int)lroundf(r->margin * 100)); text(x, y, s, 0); y += 23;
  lcd_fill_rect((uint32_t)x, (uint32_t)(y + 9), 22, 2, INK);
  text(x + 26, y, "intact", 0);
  for (int i = 0; i < 22; i += 6) lcd_fill_rect((uint32_t)(x + 96 + i), (uint32_t)(y + 9), 3, 1, INK);
  text(x + 122, y, "WCF", 0); y += 21;
  sprintf(s, "%u alloc. %s s", (unsigned)r->solves, fmt(t, abs_ms / 1000.0f, 1)); text(x, y, s, 0);
}

static void page_plot_full(void) {         /* the whole screen: F1 again goes back */
  char s[48], a[12], b[12], c[12], d[12];
  lcd_clear_buf();
  pset = px_lcd;
  clip_y = LCD_Y;
  plot_figure(LCD_X / 2, LCD_Y / 2, 117, 2);
  clip_y = BOT;
  text(LCD_X / 2 + 4, 2, "0", 0);
  text(LCD_X / 2 + 96, LCD_Y / 2 - 20, "90", 0);
  text(4, 2, vessel.name, 0);
  if (method) {
    skp_s(a, abs_res->a); skp_s(b, abs_res->b); skp_s(c, abs_res->c); skp_s(d, abs_res->d);
    text(4, 24, "ABS SKP", 0);
    sprintf(s, "(%s,%s,%s,%s)", a, b, c, d);
  } else {
    text(4, 24, "DNV L1", 0);
    sprintf(s, "(%s,%s,%s,%s)", num_s(res->A, a), num_s(res->B, b), num_s(res->C, c), num_s(res->D, d));
  }
  text(4, 46, s, 0);
  text(4, LCD_Y - 22, knots ? "kn  + m/s" : "m/s  + kn", 0);
  text(LCD_X - 78, LCD_Y - 22, "F1 back", 0);
}

static void page_plot(void) {
  if (!need_result()) return;
  if (plot_full) { page_plot_full(); return; }
  lcd_clear_buf();
  char s[64], a[12], b[12], c[12], d[12], t[16];
  snprintf(s, sizeof s, "DPCAP47  %s   . %s", vessel.name, method ? "DNV" : "ABS");
  bar(s);
  pset = px_lcd;
  int cx = 104, cy = TOP + 92, R = 88;
  plot_figure(cx, cy, R, 1);
  text(cx - 4, TOP - 3, "0", 0);
  text(cx + R - 18, cy - 9, "90", 0);
  int x = 212, y = TOP + 2;
  text(4, BOT - 20, knots ? "kn" : "m/s", 0);
  if (method) { abs_panel(x, y); return; }
  text(x, y, "DP Capability-L1", 0); y += 21;
  sprintf(s, "(%s, %s, %s, %s)", num_s(res->A, a), num_s(res->B, b), num_s(res->C, c), num_s(res->D, d));
  textinv(x + 8, y, s); y += 24;
  sprintf(s, "A bow 30   int  %s", a);  text(x, y, s, 0); y += 19;
  sprintf(s, "B all      int  %s", b);  text(x, y, s, 0); y += 19;
  sprintf(s, "C bow 30  WCSF  %s", c);  text(x, y, s, 0); y += 19;
  sprintf(s, "D all     WCSF  %s", d);  text(x, y, s, 0); y += 23;
  lcd_fill_rect((uint32_t)x, (uint32_t)(y + 9), 22, 2, INK);
  text(x + 26, y, "intact", 0);
  pset = px_lcd;
  for (int i = 0; i < 22; i += 6) lcd_fill_rect((uint32_t)(x + 96 + i), (uint32_t)(y + 9), 3, 1, INK);
  text(x + 122, y, "WCSF", 0); y += 21;
  sprintf(s, "%u allocations", (unsigned)res->solves); text(x, y, s, 0); y += 19;
  sprintf(s, "%s s at %d MHz", fmt(t, res_ms / 1000.0f, 1), res_mhz); text(x, y, s, 0);
}

/* ------------------------------------------------------------------ TABLE */
static void page_table_abs(void) {
  char s[96], t[16], u[16], w[16], z[16];
  const AbsResult *r = abs_res;
  sprintf(s, "ABS  %d-%d  wind m/s 1-min  UP DN", table_top * 10, (table_top + 7) * 10);
  bar(s);
  int y = TOP;
  sprintf(s, "from  intact  b %-5s c %-5s d grp%d", vessel.thr[r->b_thr].name, vessel.thr[r->c_thr].name, r->d_grp);
  lcd_fill_rect(0, (uint32_t)y, LCD_X, 20, INK);
  text(4, y, s, 1); y += 21;
  for (int h = table_top; h < table_top + 8 && h < NHEAD; h++) {
    sprintf(s, "%3d   %5s   %5s   %5s   %5s", h * 10, fmt(t, r->intact[h], 1), fmt(u, r->single[r->b_thr][h], 1),
            fmt(w, r->single[r->c_thr][h], 1), r->d >= 0 ? fmt(z, r->group[r->d_grp][h], 1) : "-");
    text(4, y, s, 0); y += 20;
  }
}

static void page_table(void) {
  if (!need_result()) return;
  lcd_clear_buf();
  if (method) { page_table_abs(); return; }
  char s[96], t[16], u[16];
  sprintf(s, "TABLE  %d-%d  number, wind m/s  UP DN", table_top * 10, (table_top + 7) * 10);
  bar(s);
  int y = TOP;
  sprintf(s, "from intact     WCSF   ");
  for (int g = 1; g < res->ncase && g <= 4; g++) sprintf(s + strlen(s), " g%d", g);
  lcd_fill_rect(0, (uint32_t)y, LCD_X, 20, INK);
  text(4, y, s, 1); y += 21;
  for (int h = table_top; h < table_top + 8 && h < NHEAD; h++) {
    sprintf(s, "%3d  %2d %5s   %2d %5s ", h * 10, res->num[0][h], fmt(t, res->wind[0][h], 1), res->wnum[h],
            fmt(u, res->wwind[h], 1));
    for (int g = 1; g < res->ncase && g <= 4; g++) sprintf(s + strlen(s), " %2d", res->num[g][h]);
    text(4, y, s, 0); y += 20;
  }
}

/* ------------------------------------------------------------------ LIVE */
static uint32_t live_mask(void) {
  uint32_t m = live_fail ? dp_case_mask(&vessel, live_fail) : dp_case_mask(&vessel, 0);
  return m & ~live_off;
}

static void live_solve(void) {
  Env e = dp_env_wind(live_wind);
  uint32_t t0 = sys_current_ms();
  dp_held(&vessel, live_mask(), &e, (float)live_from, &live_a);
  live_us = sys_current_ms() - t0;
}

static int bf_of(float w) { int b = 0; while (b < NBF - 1 && w > DP_TABLE[b].wind + 1e-4f) b++; return b; }

static void page_live(void) {
  lcd_clear_buf();
  char s[64], t[16], u[16];
  strcpy(s, "LIVE  UP DN dir  + - wind  1-9 thr");
  if (live_fail) sprintf(s, "LIVE  group %d lost   . next  0 all", live_fail);
  bar(s);
  pset = px_lcd;
  int cx = 104, cy = TOP + 92;
  float sc = 130.0f / vessel.lpp;
  if (sc * vessel.b > 70) sc = 70 / vessel.b;
  /* the hull */
  float L = vessel.lpp / 2, B = vessel.b / 2;
  float P[6][2] = { { L, 0 }, { L * 0.45f, B }, { -L, B * 0.92f }, { -L, -B * 0.92f }, { L * 0.45f, -B }, { L, 0 } };
  for (int i = 0; i < 5; i++)
    line(cx + (int)lroundf(P[i][1] * sc), cy - (int)lroundf(P[i][0] * sc), cx + (int)lroundf(P[i + 1][1] * sc),
         cy - (int)lroundf(P[i + 1][0] * sc), 0);
  /* the weather, coming from live_from */
  float a = live_from * PI_F / 180.0f;
  arrow(cx + (int)lroundf(90 * sinf(a)), cy - (int)lroundf(90 * cosf(a)), cx + (int)lroundf(72 * sinf(a)),
        cy - (int)lroundf(72 * cosf(a)));
  /* the thrusters and their forces */
  uint32_t m = live_mask();
  for (int i = 0; i < vessel.nthr; i++) {
    const Thruster *th = &vessel.thr[i];
    int x = cx + (int)lroundf(th->y * sc), y = cy - (int)lroundf(th->x * sc);
    if (m >> i & 1) {
      disc(x, y, 3);
      float fx = live_a.f[i][0], fy = live_a.f[i][1], fm = sqrtf(fx * fx + fy * fy);
      if (fm > 0.5f) {
        float l = 5 + live_a.use[i] * 26;
        arrow(x, y, x + (int)lroundf(fy / fm * l), y - (int)lroundf(fx / fm * l));
      }
    } else { circle(x, y, 4, 0); line(x - 4, y - 4, x + 4, y + 4, 0); line(x - 4, y + 4, x + 4, y - 4, 0); }
  }
  /* the right side: the weather, then each thruster's use */
  int X = 212, y = TOP;
  Env e = dp_env_wind(live_wind);
  sprintf(s, "from %03d  wind %s", live_from, fmt(t, live_wind, 1)); text(X, y, s, 0); y += 19;
  sprintf(s, "BF%d  Hs %s  c %s", bf_of(live_wind), fmt(t, e.hs, 1), fmt(u, e.cur, 2)); text(X, y, s, 0); y += 21;
  for (int i = 0; i < vessel.nthr && i < 8; i++) {
    const Thruster *th = &vessel.thr[i];
    sprintf(s, "%d %s", i + 1, th->name);
    text(X, y, s, 0);
    int bx = X + 74, bw = 80;
    lcd_fill_rect((uint32_t)bx, (uint32_t)(y + 4), (uint32_t)bw, 1, INK);
    lcd_fill_rect((uint32_t)bx, (uint32_t)(y + 14), (uint32_t)bw, 1, INK);
    lcd_fill_rect((uint32_t)bx, (uint32_t)(y + 4), 1, 11, INK);
    lcd_fill_rect((uint32_t)(bx + bw - 1), (uint32_t)(y + 4), 1, 11, INK);
    if (m >> i & 1) {
      int w = (int)lroundf(live_a.use[i] * (bw - 4));
      if (w > 0) lcd_fill_rect((uint32_t)(bx + 2), (uint32_t)(y + 6), (uint32_t)w, 7, INK);
      sprintf(s, "%d", (int)lroundf(live_a.use[i] * 100));
    } else strcpy(s, "off");
    text(bx + bw + 4, y, s, 0);
    y += 17;
  }
  y += 2;
  if (live_a.ok) strcpy(s, "HELD");
  else if (!live_a.power_ok) strcpy(s, "NOT HELD: power");
  else sprintf(s, "NOT HELD: %d %% short", (int)lroundf(live_a.res * 100) < 1 ? 1 : (int)lroundf(live_a.res * 100));
  if (live_a.ok) text(X, y, s, 0); else textinv(X, y, s);
  sprintf(s, "%d it %u ms", live_a.iters, (unsigned)live_us);
  text(LCD_X - tw(s) - 2, y, live_a.ok ? s : "", 0);
  text(4, BOT - 20, "ENTER limit  . group", 0);
}

/* ------------------------------------------------------------------ MAP */
static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static void shade(int x0, int y0, int w, int h, int level) {   /* level 0 white .. 16 black */
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++) if (BAYER[y & 3][x & 3] < level) pset(x, y);
}

static void page_map(void) {
  lcd_clear_buf();
  bar("DPCAP47  map: current x direction");
  pset = px_lcd;
  char s[64], t[16];
  if (!have_map) {
    text(20, 50, "The limiting wind for each direction", 0);
    text(20, 72, "and each current from 0 to 2 m/s:", 0);
    text(20, 94, "36 x 17 = 612 searches, intact. Slow:",  0);
    text(20, 116, "several times the PLOT's time.", 0);
    textinv(20, 146, "ENTER: compute   EXIT: stop");
    return;
  }
  int x0 = 36, y0 = TOP + 4, cw = 10, ch = 8;
  for (int h = 0; h < NHEAD; h++)
    for (int c = 0; c < NCUR; c++) {
      int b = bf_of(map[h * NCUR + c] - 0.0001f);
      if (map[h * NCUR + c] >= DP_TABLE[NBF - 1].wind) b = NBF - 1;
      shade(x0 + h * cw, y0 + (NCUR - 1 - c) * ch, cw, ch, (NBF - 1 - b) * 16 / (NBF - 1));
    }
  lcd_fill_rect((uint32_t)x0, (uint32_t)(y0 + NCUR * ch), NHEAD * cw, 1, INK);
  lcd_fill_rect((uint32_t)(x0 - 1), (uint32_t)y0, 1, NCUR * ch, INK);
  int y75 = y0 + (NCUR - 1 - 6) * ch + ch / 2;                  /* Table 2-1's 0.75 m/s */
  lcd_fill_rect((uint32_t)(x0 - 6), (uint32_t)y75, 5, 1, INK);
  text(2, y0 - 2, "2.0", 0);
  text(2, y75 - 9, ".75", 0);
  text(2, y0 + NCUR * ch - 18, "0", 0);
  for (int d = 0; d < 360; d += 90) { sprintf(s, "%d", d); text(x0 + d / 10 * cw - (d ? 8 : 0), y0 + NCUR * ch + 1, s, 0); }
  text(x0 + 33 * cw, y0 + NCUR * ch + 1, "deg", 0);
  int ly = BOT - 20;
  text(4, ly, "BF 0", 0);
  for (int b = 0; b <= 11; b++) {
    int lx = 44 + b * 13;
    shade(lx, ly + 3, 12, 12, (NBF - 1 - b) * 16 / (NBF - 1));
    lcd_fill_rect((uint32_t)lx, (uint32_t)(ly + 3), 12, 1, INK); lcd_fill_rect((uint32_t)lx, (uint32_t)(ly + 14), 12, 1, INK);
    lcd_fill_rect((uint32_t)lx, (uint32_t)(ly + 3), 1, 12, INK); lcd_fill_rect((uint32_t)(lx + 11), (uint32_t)(ly + 3), 1, 12, INK);
  }
  text(44 + 12 * 13 + 2, ly, "11", 0);
  sprintf(s, "%u in %s s", (unsigned)map_solves, fmt(t, map_ms / 1000.0f, 1));
  text(LCD_X - tw(s) - 4, ly, s, 0);
}

/* ------------------------------------------------------------------ VESSEL */
static void page_vessel(void) {
  lcd_clear_buf();
  char s[80], a[12], b[12], c[12];
  snprintf(s, sizeof s, "DPCAP47  %s", vfile[0] ? vfile : "example (built in)");
  bar(s);
  int y = TOP;
  snprintf(s, sizeof s, "%s  Lpp %s B %s T %s", vessel.name, fmt(a, vessel.lpp, 1), fmt(b, vessel.b, 1), fmt(c, vessel.t, 1));
  text(4, y, s, 0); y += 19;
  sprintf(s, "wind %s/%s m2 at %s  cur %s m2", fmt(a, vessel.afw, 0), fmt(b, vessel.alw, 0), fmt(c, vessel.xlw, 1),
          fmt(c + 6, vessel.alc, 0));
  text(4, y, s, 0); y += 21;
  if (vessel.assumed) {                     /* what the file did not give */
    sprintf(s, "assumed:%s%s%s%s", vessel.assumed & VA_LOS ? " Los" : "", vessel.assumed & VA_XLOS ? " xLos" : "",
            vessel.assumed & VA_BOW ? " bow 35" : "", vessel.assumed & VA_CWL ? " CWLaft 1" : "");
    textinv(4, y, s); y += 21;
  }
  lcd_fill_rect(0, (uint32_t)y, LCD_X, 20, INK);
  text(4, y, "  name  type   x m   y m  grp  kN", 1); y += 21;
  for (int i = vessel_top; i < vessel.nthr && y < BOT - 18; i++) {
    const Thruster *t = &vessel.thr[i];
    sprintf(s, "%d %-5s %-4s %5s %5s  %d  %s", i + 1, t->name, DP_TYPE[t->type], fmt(a, t->x, 1), fmt(b, t->y, 1), t->grp,
            fmt(c, t->tf, 0));
    if (t->type == T_PROP) sprintf(s + strlen(s), "/%s", fmt(a, t->tr, 0));
    if (t->flags & TF_NO_D) strcat(s, " no D");
    if (t->flags & TF_NO_AR) strcat(s, " no Ar");
    text(4, y, s, 0); y += 19;
  }
}

/* ------------------------------------------------------------------ PRINT: HP 82240 over infrared */
#define PW 166                              /* the printer's dots a line */
static uint8_t *pbuf;                       /* PW x PW bits, heap */
static void px_pr(int x, int y) { if ((unsigned)x < PW && (unsigned)y < PW) pbuf[y * (PW / 8 + 1) + x / 8] |= (uint8_t)(0x80 >> (x & 7)); }

static void pr_wait(void) { for (int i = 0; i < 400 && !print_is_ready(); i++) sys_delay(5); }

static void pr_text(const char *s) {
  pr_wait();
  while (*s) print_byte((uint8_t)*s++);
  printer_advance_buf(PRINT_TXT_LN);
  for (int i = 0; i < 400 && printer_busy_for(PRINT_TXT_LN); i++) sys_delay(5);
}

static void print_all(void) {
  if (!have_method()) compute_method();
  if (!have_method()) return;
  pbuf = calloc(PW, PW / 8 + 1);
  if (!pbuf) { strcpy(msg, "no memory to print"); return; }
  busy_screen("DPCAP47: printing (IR)", "HP 82240 printer");
  char s[64], a[12], b[12], c[12], d[12];
  if (method) {
    const AbsResult *r = abs_res;
    pr_text("DPCAP47 ABS DPS Guide");
    snprintf(s, sizeof s, "%s", vessel.name); pr_text(s);
    skp_s(a, r->a); skp_s(b, r->b); skp_s(c, r->c); skp_s(d, r->d);
    sprintf(s, "SKP(%s,%s,%s,%s,%d.%d,NS)", a, b, c, d, abs_cur10 / 10, abs_cur10 % 10); pr_text(s);
    sprintf(s, "b %s  c %s  d grp %d", vessel.thr[r->b_thr].name, vessel.thr[r->c_thr].name, r->d_grp); pr_text(s);
    sprintf(s, "margin %d%%, wind 1-min", abs_margin); pr_text(s);
  } else {
    pr_text("DPCAP47 DNV-ST-0111 L1");
    snprintf(s, sizeof s, "%s", vessel.name); pr_text(s);
    sprintf(s, "DP Capability-L1"); pr_text(s);
    sprintf(s, "  (%s, %s, %s, %s)", num_s(res->A, a), num_s(res->B, b), num_s(res->C, c), num_s(res->D, d)); pr_text(s);
  }
  pset = px_pr;
  plot_figure(PW / 2, PW / 2, PW / 2 - 2, 0);
  for (int y = 0; y < PW; y += 8) {                     /* a graphics line: ESC n, then n columns of 8 dots */
    pr_wait();
    print_byte(27); print_byte(PW);
    for (int x = 0; x < PW; x++) {
      uint8_t col = 0;
      for (int k = 0; k < 8; k++) if (y + k < PW && pbuf[(y + k) * (PW / 8 + 1) + x / 8] & (0x80 >> (x & 7))) col |= (uint8_t)(1 << k);
      print_byte(col);
    }
    printer_advance_buf(PRINT_GRA_LN);
    for (int i = 0; i < 400 && printer_busy_for(PRINT_GRA_LN); i++) sys_delay(5);
    progress(y / 8 + 1, (PW + 7) / 8);
  }
  pset = px_lcd;
  free(pbuf); pbuf = NULL;
  if (method) {
    const AbsResult *r = abs_res;
    pr_text("from  intact   WCF m/s");
    for (int h = 0; h < NHEAD; h += 3) {
      char t[12], u[12];
      sprintf(s, "%3d   %5s  %5s", h * 10, fmt(t, r->intact[h], 1), r->d >= 0 ? fmt(u, r->group[r->d_grp][h], 1) : "-");
      pr_text(s);
    }
  } else {
    pr_text("from  int   m/s  WCSF");
    for (int h = 0; h < NHEAD; h += 3) {
      char t[12], u[12];
      sprintf(s, "%3d  %2d %5s  %2d %5s", h * 10, res->num[0][h], fmt(t, res->wind[0][h], 1), res->wnum[h], fmt(u, res->wwind[h], 1));
      pr_text(s);
    }
  }
  pr_text("");
  strcpy(msg, "printed");
}

/* ------------------------------------------------------------------ about, menu, setup */
static void about(void) {
  lcd_clear_buf();
  bar("DPCAP47 " PROGRAM_VERSION);
  int y = TOP + 2;
  static const char *const L[] = {
    "DP capability: DNV-ST-0111 (Dec 2021) level 1,",
    "and ABS SKP. (C) 2026 Victor Valdes, GPLv3.",
    "Not DNV's or ABS's software, not approved:",
    "not for class, contracts or operations.",
    "Vessel files (.DPV) in " DIR,
    "Written with the help of AI (see README)",
  };
  for (unsigned i = 0; i < sizeof L / sizeof *L; i++) { text(4, y, L[i], 0); y += 21; }
  char s[64];
  sprintf(s, "Free heap %d KB   CPU %s", sys_free_mem() / 1024, usb_powered() ? "160 MHz (USB)" : "80 MHz (battery)");
  text(4, y + 4, s, 0);
  lcd_refresh();
  wait_release();
  for (;;) { int k = wait_key(); if (!k || k == KEY_EXIT || k == KEY_ENTER) break; }
}

#define MI_DP_ABOUT 1
static const uint8_t setup_items[] = { MI_SET_TIME, MI_SET_DATE, MI_BEEP_MUTE, MI_MSC, MI_SYSTEM_ENTER, MI_DP_ABOUT, 0 };
static const smenu_t SETUP_MENU = { "DPCAP47 Setup", setup_items, NULL, NULL };
static int run_menu_item(uint8_t id) { if (id == MI_DP_ABOUT) { about(); return 0; } return MRET_UNIMPL; }
static const char *menu_line_str(uint8_t id, char *s, const int slen) {
  (void)s; (void)slen;
  return id == MI_DP_ABOUT ? "About DPCAP47 >" : NULL;
}

static int menu_off;

static void menu_run(void) {
  for (;;) {
    lcd_clear_buf();
    bar("DPCAP47 " PROGRAM_VERSION "  menu      EXIT: back");
    char s[80];
    int y = TOP + 4;
    snprintf(s, sizeof s, "vessel: %s", vfile[0] ? vfile : "the example (built in)"); text(4, y, s, 0); y += 26;
    text(4, y, "1 load a vessel file (.DPV)", 0); y += 21;
    text(4, y, "2 write the example to " EX_FILE, 0); y += 21;
    text(4, y, "3 use the example", 0); y += 21;
    text(4, y, "4 setup (time, USB disk, System)", 0); y += 21;
    text(4, y, "5 about", 0); y += 21;
    char t[12];
    sprintf(s, "6 method: %s", method ? "ABS SKP" : "DNV-ST-0111 L1"); text(4, y, s, 0); y += 21;
    sprintf(s, "7 ABS current %s kn   8 ABS margin %d %%", fmt(t, abs_cur10 / 10.0f, 1), abs_margin);
    text(4, y, s, 0); y += 21;
    if (msg[0]) textinv(4, y, msg);
    static const char *K[6] = { "", "", "", "", "", "OFF" };
    lcd_draw_menu_keys(K);
    lcd_refresh();
    int k = wait_key();
    msg[0] = 0;
    switch (k) {
    case 0: case KEY_EXIT: case KEY_ENTER: return;
    case KEY_1:
      check_create_dir(DIR);
      file_selection_screen("Vessel file", DIR, ".DPV", pick_cb, 0, 0, NULL);
      if (!msg[0]) { cfg_save(); strcpy(msg, "loaded"); page = P_VESSEL; return; }
      break;
    case KEY_2: write_file(EX_FILE, DP_EXAMPLE); strcpy(msg, "written: copy it and edit it on the PC"); break;
    case KEY_3: vessel_load(""); cfg_save(); strcpy(msg, "the example"); break;
    case KEY_4: wait_release(); handle_menu(&SETUP_MENU, MENU_RESET, 0); wait_release(); break;
    case KEY_5: about(); break;
    case KEY_6: method = !method; cfg_save(); break;
    case KEY_7: abs_cur10 = abs_cur10 >= 30 ? 5 : abs_cur10 + 5; have_abs = 0; cfg_save(); break;   /* 0.5 .. 3 kn */
    case KEY_8: abs_margin = abs_margin >= 30 ? 0 : abs_margin + 10; have_abs = 0; cfg_save(); break;
    case KEY_F6: menu_off = 1; return;
    }
  }
}

/* ------------------------------------------------------------------ the pages */
static void draw(void) {
  switch (page) {
  case P_PLOT: page_plot(); break;
  case P_TABLE: page_table(); break;
  case P_LIVE: page_live(); break;
  case P_MAP: page_map(); break;
  default: page_vessel(); break;
  }
  if (msg[0] && page != P_LIVE) { lcd_fill_rect(0, BOT - 21, LCD_X, 21, 0); textinv(4, BOT - 21, msg); msg[0] = 0; }
  static const char *K[6] = { "PLOT", "TABLE", "LIVE", "MAP", "VESL", "PRINT" };
  if (page == P_PLOT && plot_full && have_method()) { lcd_refresh(); return; }
  lcd_draw_menu_keys(K);
  lcd_refresh();
}

static void live_key(int k) {
  switch (k) {
  case KEY_UP: live_from = (live_from + 10) % 360; break;
  case KEY_DOWN: live_from = (live_from + 350) % 360; break;
  case KEY_ADD: live_wind += 1.0f; break;
  case KEY_SUB: live_wind -= 1.0f; break;
  case KEY_MUL: live_wind += 0.1f; break;
  case KEY_DIV: live_wind -= 0.1f; break;
  case KEY_0: live_off = 0; live_fail = 0; break;
  case KEY_DOT: live_fail = vessel.ngrp > 1 ? (live_fail + 1) % (vessel.ngrp + 1) : 0; break;
  case KEY_ENTER: {                         /* the limit for this direction, now */
    uint32_t t0 = sys_current_ms();
    live_wind = dp_limit_wind(&vessel, live_mask(), (float)live_from, -1, -1, &live_a);
    live_solve();
    live_us = sys_current_ms() - t0;
    break;
  }
  default: {
    static const int D[9] = { KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9 };
    for (int i = 0; i < 9; i++) if (k == D[i] && i < vessel.nthr) live_off ^= 1u << i;
  }
  }
  if (live_wind < 0) live_wind = 0;
  if (live_wind > 40) live_wind = 40;
  if (k != KEY_ENTER) live_solve();
}

/* ------------------------------------------------------------------ power, as the DMCP SDK's template */
static void power_off(void) {
  lcd_refresh_wait();
  SET_ST(STAT_PGM_END);
  if (!ST(STAT_SUSPENDED)) {
    lcd_set_buf_cleared(0);
    draw_power_off_image(1);
    LCD_power_off(0);
    SET_ST(STAT_SUSPENDED);
    SET_ST(STAT_OFF);
  }
  while (ST(STAT_PGM_END) && ST(STAT_SUSPENDED)) { CLR_ST(STAT_RUNNING); sys_sleep(); }
  SET_ST(STAT_RUNNING);
  if (ST(STAT_OFF)) { LCD_power_on(); rtc_wakeup_delay(); CLR_ST(STAT_OFF); }
  key_pop_all();
  draw();
}

void _exit(int code) { (void)code; sys_reset(); for (;;) {} }

void program_main(void) {
  switch_usb_powered_freq();
  run_menu_item_app = run_menu_item;
  menu_line_str_app = menu_line_str;
  pset = px_lcd;
  dp_maxit = 200;                           /* the calculator: quicker; the PC uses 600 (winds within 0.1 m/s) */
  res = malloc(sizeof *res);
  abs_res = malloc(sizeof *abs_res);
  map = malloc(sizeof(float) * NHEAD * NCUR);
  vessel_load("");
  cfg_load();
  if (!file_exists(EX_FILE)) write_file(EX_FILE, DP_EXAMPLE);
  live_solve();
  draw();
  for (;;) {
    int k = wait_key();
    if (ST(STAT_POWER_CHANGE)) { switch_usb_powered_freq(); CLR_ST(STAT_POWER_CHANGE); }
    if (!k) continue;
    switch (k) {
    case KEY_SCREENSHOT: create_screenshot(1); continue;
    case KEY_F1: plot_full = page == P_PLOT && have_method() ? !plot_full : 0; page = P_PLOT; break;
    case KEY_F2: page = P_TABLE; break;
    case KEY_F3: page = P_LIVE; live_solve(); break;
    case KEY_F4: page = P_MAP; break;
    case KEY_F5: page = P_VESSEL; break;
    case KEY_F6: print_all(); break;
    case KEY_EXIT:
      wait_release();
      menu_run();
      if (menu_off) { menu_off = 0; power_off(); continue; }
      break;
    default:
      if (page == P_LIVE) live_key(k);
      else if (page == P_TABLE && k == KEY_DOWN) table_top = table_top + 8 < NHEAD ? table_top + 8 : table_top;
      else if (page == P_TABLE && k == KEY_UP) table_top = table_top >= 8 ? table_top - 8 : 0;
      else if (page == P_MAP && k == KEY_ENTER) compute_map();
      else if ((page == P_PLOT || page == P_TABLE) && k == KEY_ENTER) compute_method();
      else if (page == P_PLOT && k == KEY_ADD) { knots = !knots; cfg_save(); }
      else if ((page == P_PLOT || page == P_TABLE) && k == KEY_DOT) { method = !method; cfg_save(); }
      else if (page == P_VESSEL && k == KEY_DOWN && vessel_top + 1 < vessel.nthr) vessel_top++;
      else if (page == P_VESSEL && k == KEY_UP && vessel_top > 0) vessel_top--;
      else continue;
    }
    draw();
  }
}
