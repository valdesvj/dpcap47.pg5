#!/usr/bin/env python3
"""DPCAP47 on the PC (GTK 4): the calculator's DP capability engine, and the methods side by side.

  make lib && python3 tools/dpcap47_gtk.py [vessel.DPV] [--methods=l1,l2,site,abs] [--shot=DIR]

Methods: DNV-ST-0111 level 1 (the calculator's own engine and numbers), level 2 (the vessel's own coefficient
tables, a dynamic factor), level 2-site (a site's weather), and the ABS DPS Guide's station keeping performance
(SKP). Pages: the plot (all methods over each other), the table, live (the allocation for one weather), the current
map, and the vessel file (edit it here, Apply). Export: the plot as PNG, everything as CSV.
Needs PyGObject with GTK 4 (Arch: pacman -S python-gobject gtk4). Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
"""
import csv
import math
import os
import sys
import threading
import time

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib, Gio  # noqa: E402
import cairo  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dpcap import engine as E   # noqa: E402
from dpcap import methods as M  # noqa: E402

COLORS = {"l1": (0.10, 0.35, 0.80), "l2": (0.10, 0.60, 0.25), "site": (0.90, 0.50, 0.05), "abs": (0.80, 0.10, 0.15)}
NAMES = {"l1": "DNV L1 (calculator)", "l2": "DNV L2", "site": "DNV L2-site", "abs": "ABS SKP"}
KT = 1 / 0.514444


def polar_plot(cr, W, H, results, show, unit_kt, title):
    """The capability plot: each method's envelopes; show: 'intact', 'wcsf' or 'both'."""
    cr.set_source_rgb(1, 1, 1)
    cr.paint()
    cx, cy = W * 0.62, H * 0.53
    R = min(W * 0.35, H * 0.42)
    top = 40.0 if not unit_kt else 80.0
    k = (KT if unit_kt else 1.0)
    cr.set_line_width(1)
    cr.select_font_face("Sans")
    cr.set_font_size(11)
    step = 10 if not unit_kt else 20
    for wv in range(step, int(top) + 1, step):
        r = R * wv / top
        cr.set_source_rgb(0.75, 0.75, 0.75)
        cr.set_dash([2, 3])
        cr.arc(cx, cy, r, 0, 2 * math.pi)
        cr.stroke()
        cr.set_dash([])
        cr.set_source_rgb(0.4, 0.4, 0.4)
        cr.move_to(cx + 3, cy - r - 2)
        cr.show_text("%d" % wv)
    for d in range(0, 360, 30):
        a = math.radians(d)
        cr.set_source_rgb(0.8, 0.8, 0.8)
        cr.move_to(cx, cy)
        cr.line_to(cx + R * math.sin(a), cy - R * math.cos(a))
        cr.stroke()
        cr.set_source_rgb(0.2, 0.2, 0.2)
        tx, ty = cx + (R + 14) * math.sin(a), cy - (R + 14) * math.cos(a)
        s = "%d°" % d
        ext = cr.text_extents(s)
        cr.move_to(tx - ext.width / 2, ty + ext.height / 2)
        cr.show_text(s)
    # the hull, bow up
    L, B = R * 0.16, R * 0.05
    cr.set_source_rgb(0.3, 0.3, 0.3)
    cr.move_to(cx, cy - L)
    for px, py in ((B, -L * 0.45), (B, L), (-B, L), (-B, -L * 0.45), (0, -L)):
        cr.line_to(cx + px, cy + py)
    cr.stroke()
    legend = []
    for key, res in results.items():
        col = COLORS[key]
        kinds = [("intact", [], 2.4), ("wcsf", [7, 4], 1.6)]
        for kind, dash, lw in kinds:
            if show != "both" and show != kind:
                continue
            c = res.case(kind)
            if c is None:
                continue
            cr.set_source_rgb(*col)
            cr.set_line_width(lw)
            cr.set_dash(dash)
            for h in range(E.NHEAD + 1):
                wv = min(c.winds[h % E.NHEAD] * k, top)
                a = math.radians((h % E.NHEAD) * 10)
                x, y = cx + R * wv / top * math.sin(a), cy - R * wv / top * math.cos(a)
                (cr.move_to if h == 0 else cr.line_to)(x, y)
            cr.stroke()
            cr.set_dash([])
            legend.append((col, dash, lw, "%s, %s" % (NAMES[key], "intact" if kind == "intact" else "worst failure")))
    # the legend and the summaries
    cr.set_font_size(13)
    cr.set_source_rgb(0, 0, 0)
    cr.move_to(10, 20)
    cr.show_text(title)
    cr.set_font_size(11)
    x0, y = 46, 44
    cr.move_to(10, H - 10)
    cr.set_source_rgb(0.3, 0.3, 0.3)
    cr.show_text("limiting wind, %s  (DNV: 10-min mean, ABS: 1-min mean)" % ("knots" if unit_kt else "m/s"))
    for col, dash, lw, s in legend:
        cr.set_source_rgb(*col)
        cr.set_line_width(lw)
        cr.set_dash(dash)
        cr.move_to(x0 - 30, y - 4)
        cr.line_to(x0 - 6, y - 4)
        cr.stroke()
        cr.set_dash([])
        cr.set_source_rgb(0, 0, 0)
        cr.move_to(x0, y)
        cr.show_text(s)
        y += 18
    y += 10
    for key, res in results.items():
        cr.set_source_rgb(*COLORS[key])
        cr.move_to(x0 - 36, y)
        cr.show_text(res.summary)
        y += 18


class App(Gtk.Application):
    def __init__(self, path, shot=None, methods=None):
        super().__init__(application_id="org.valdes.dpcap47", flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.path, self.shot, self.want = path, shot, methods
        self.text = open(path).read() if path else E.example_text()
        self.results = {}
        self.busy = False
        self.live_alloc = E.Alloc()

    # ------------------------------------------------------------------ building the window
    def do_activate(self):
        win = Gtk.ApplicationWindow(application=self, title="DPCAP47 — DP capability on the PC")
        win.set_default_size(1280, 800)
        self.win = win
        hb = Gtk.HeaderBar()
        win.set_titlebar(hb)
        for label, cb in (("Open vessel…", self.on_open), ("Export PNG", self.on_png), ("Export CSV", self.on_csv)):
            b = Gtk.Button(label=label)
            b.connect("clicked", cb)
            hb.pack_start(b) if label.startswith("Open") else hb.pack_end(b)

        paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        win.set_child(paned)
        side = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        for m in ("margin_top", "margin_bottom", "margin_start", "margin_end"):
            side.set_property(m, 10)
        side.set_size_request(360, -1)
        paned.set_start_child(side)
        paned.set_position(380)
        paned.set_shrink_start_child(False)

        self.vlabel = Gtk.Label(xalign=0, wrap=True)
        side.append(self.vlabel)
        side.append(Gtk.Separator())

        def row(*w):
            b = Gtk.Box(spacing=6)
            for x in w:
                b.append(x)
            side.append(b)

        def spin(lo, hi, step, val, digits):
            s = Gtk.SpinButton.new_with_range(lo, hi, step)
            s.set_digits(digits)
            s.set_value(val)
            return s

        self.cb = {}
        for key in ("l1", "l2", "site", "abs"):
            self.cb[key] = Gtk.CheckButton(label=NAMES[key])
            self.cb[key].set_active(key in (self.want or ("l1", "abs")))
        side.append(self.cb["l1"])
        self.dyn = spin(1.0, 2.0, 0.05, 1.25, 2)
        row(self.cb["l2"], Gtk.Label(label="  dyn. factor"), self.dyn)
        side.append(self.cb["site"])
        side.append(self.cb["abs"])
        self.abs_cur = spin(0, 5, 0.1, 1.5, 1)
        self.abs_margin = spin(0, 50, 5, 20, 0)
        row(Gtk.Label(label="    ABS current kn"), self.abs_cur, Gtk.Label(label="margin %"), self.abs_margin)
        self.abs_thr = Gtk.CheckButton(label="    ABS bollard pull from the diameters")
        self.abs_thr.set_active(True)
        side.append(self.abs_thr)
        self.compute_btn = Gtk.Button(label="Compute")
        self.compute_btn.connect("clicked", lambda *_: self.compute())
        side.append(self.compute_btn)
        self.status = Gtk.Label(xalign=0, wrap=True)
        side.append(self.status)
        side.append(Gtk.Separator())
        self.summary = Gtk.Label(xalign=0, wrap=True, selectable=True)
        side.append(self.summary)
        sw = Gtk.ScrolledWindow(vexpand=True)
        self.notes = Gtk.Label(xalign=0, yalign=0, wrap=True, selectable=True)
        sw.set_child(self.notes)
        side.append(sw)

        nb = Gtk.Notebook()
        paned.set_end_child(nb)
        self.nb = nb
        # the plot
        pbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        bar = Gtk.Box(spacing=8)
        for m in ("margin_top", "margin_start"):
            bar.set_property(m, 6)
        self.show = Gtk.DropDown.new_from_strings(["intact and worst failure", "intact", "worst failure"])
        self.show.connect("notify::selected", lambda *_: self.plot.queue_draw())
        self.unit = Gtk.DropDown.new_from_strings(["m/s", "knots"])
        self.unit.connect("notify::selected", lambda *_: self.plot.queue_draw())
        bar.append(Gtk.Label(label="show"))
        bar.append(self.show)
        bar.append(Gtk.Label(label="wind in"))
        bar.append(self.unit)
        pbox.append(bar)
        self.plot = Gtk.DrawingArea(hexpand=True, vexpand=True)
        self.plot.set_draw_func(self.draw_plot)
        pbox.append(self.plot)
        nb.append_page(pbox, Gtk.Label(label="Plot"))
        # the table
        self.table = Gtk.TextView(editable=False, monospace=True)
        sw = Gtk.ScrolledWindow()
        sw.set_child(self.table)
        nb.append_page(sw, Gtk.Label(label="Table"))
        # live
        lbox = Gtk.Box(spacing=8)
        lside = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        for m in ("margin_top", "margin_start", "margin_end"):
            lside.set_property(m, 10)
        self.live_method = Gtk.DropDown.new_from_strings(["DNV L1 forces and thrust", "ABS forces and thrust"])
        self.live_dir = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 350, 10)
        self.live_dir.set_value(90)
        self.live_wind = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 40, 0.1)
        self.live_wind.set_value(15)
        for w_ in (self.live_dir, self.live_wind):
            w_.set_draw_value(True)
            w_.set_size_request(260, -1)
            w_.connect("value-changed", lambda *_: self.live_update())
        self.live_method.connect("notify::selected", lambda *_: self.live_update())
        lside.append(Gtk.Label(label="weather from (deg)", xalign=0))
        lside.append(self.live_dir)
        lside.append(Gtk.Label(label="wind (m/s)", xalign=0))
        lside.append(self.live_wind)
        lside.append(self.live_method)
        lim = Gtk.Button(label="Find the limit")
        lim.connect("clicked", self.live_limit)
        lside.append(lim)
        self.thr_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        lside.append(Gtk.Label(label="thrusters working", xalign=0))
        lside.append(self.thr_box)
        self.live_label = Gtk.Label(xalign=0, wrap=True)
        lside.append(self.live_label)
        lbox.append(lside)
        self.live_area = Gtk.DrawingArea(hexpand=True, vexpand=True)
        self.live_area.set_draw_func(self.draw_live)
        lbox.append(self.live_area)
        nb.append_page(lbox, Gtk.Label(label="Live"))
        # the map
        mbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        mb = Gtk.Button(label="Compute the map (DNV L1: 36 directions × 17 currents)")
        mb.connect("clicked", self.on_map)
        mbox.append(mb)
        self.map = None
        self.map_area = Gtk.DrawingArea(hexpand=True, vexpand=True)
        self.map_area.set_draw_func(self.draw_map)
        mbox.append(self.map_area)
        nb.append_page(mbox, Gtk.Label(label="Map"))
        # the vessel file
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.editor = Gtk.TextView(monospace=True)
        sw = Gtk.ScrolledWindow(vexpand=True)
        sw.set_child(self.editor)
        vbox.append(sw)
        ab = Gtk.Button(label="Apply (read the file again and compute)")
        ab.connect("clicked", self.on_apply)
        vbox.append(ab)
        nb.append_page(vbox, Gtk.Label(label="Vessel file"))

        self.load_text(self.text, self.path)
        win.present()
        self.compute()

    # ------------------------------------------------------------------ the vessel
    def load_text(self, text, path):
        try:
            v = E.parse(text)
        except ValueError as e:
            self.status.set_text("Vessel file: " + str(e))
            return False
        self.v, self.ex, self.text, self.path = v, M.Extras(text), text, path
        self.editor.get_buffer().set_text(text)
        self.cb["site"].set_sensitive(bool(self.ex.site))
        if not self.ex.site:
            self.cb["site"].set_active(False)
        ts = E.thrusters(v)
        self.vlabel.set_markup("<b>%s</b>\n%s\nLpp %.1f  B %.1f  T %.1f m, %d thrusters, %d groups%s" % (
            GLib.markup_escape_text(v.name.decode()), GLib.markup_escape_text(os.path.basename(path) if path else
                                                                              "the example (built in)"),
            v.lpp, v.b, v.t, v.nthr, v.ngrp, ", level 2 tables" if self.ex.level2() else ""))
        child = self.thr_box.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.thr_box.remove(child)
            child = nxt
        self.thr_checks = []
        for i, t in enumerate(ts):
            c = Gtk.CheckButton(label="%d %s  %s  %.0f kN" % (i + 1, t.name.decode(), E.TYPES[t.type], t.tf))
            c.set_active(True)
            c.connect("toggled", lambda *_: self.live_update())
            self.thr_box.append(c)
            self.thr_checks.append(c)
        self.results = {}
        self.map = None
        return True

    def on_apply(self, *_):
        b = self.editor.get_buffer()
        if self.load_text(b.get_text(b.get_start_iter(), b.get_end_iter(), False), self.path):
            self.compute()

    def on_open(self, *_):
        d = Gtk.FileDialog(title="Open a vessel file")
        f = Gtk.FileFilter()
        f.set_name("DPCAP47 vessel files (*.DPV)")
        f.add_pattern("*.DPV")
        f.add_pattern("*.dpv")
        fl = Gio.ListStore.new(Gtk.FileFilter)
        fl.append(f)
        d.set_filters(fl)

        def done(dlg, res):
            try:
                gf = dlg.open_finish(res)
            except GLib.Error:
                return
            p = gf.get_path()
            if self.load_text(open(p).read(), p):
                self.compute()
        d.open(self.win, None, done)

    # ------------------------------------------------------------------ computing, in a thread
    def compute(self):
        if self.busy:
            return
        self.busy = True
        self.compute_btn.set_sensitive(False)
        self.status.set_text("computing…")
        want = [k for k in ("l1", "l2", "site", "abs") if self.cb[k].get_active()]
        args = dict(dyn=self.dyn.get_value(), cur=self.abs_cur.get_value(), margin=self.abs_margin.get_value() / 100,
                    thr=self.abs_thr.get_active())
        v, ex = E.copy(self.v), self.ex

        def work():
            out, times, err = {}, [], None
            for k in want:
                t0 = time.time()
                try:
                    if k == "l1":
                        out[k] = M.dnv_l1(v, ex)
                    elif k == "l2":
                        out[k] = M.dnv_l2(v, ex, args["dyn"])
                    elif k == "site":
                        out[k] = M.dnv_l2(v, ex, args["dyn"], site=True)
                    else:
                        out[k] = M.abs_skp(v, ex, args["cur"], args["margin"], args["thr"])
                except ValueError as e:
                    err = str(e)
                times.append("%s %.2f s (%d allocations)" % (NAMES[k], time.time() - t0, out[k].solves if k in out else 0))
            GLib.idle_add(self.computed, out, times, err)
        threading.Thread(target=work, daemon=True).start()

    def computed(self, out, times, err):
        self.busy = False
        self.compute_btn.set_sensitive(True)
        self.results = out
        self.status.set_text(("error: " + err + "\n" if err else "") + "\n".join(times))
        self.summary.set_markup("\n".join('<span foreground="#%02x%02x%02x"><b>%s</b></span>\n%s' % (
            *(int(255 * c) for c in COLORS[k]), NAMES[k], GLib.markup_escape_text(r.summary)) for k, r in out.items()))
        self.notes.set_text("\n\n".join("%s:\n- %s" % (NAMES[k], "\n- ".join(r.notes)) for k, r in out.items()))
        self.fill_table()
        self.plot.queue_draw()
        self.live_update()
        if self.shot:
            GLib.timeout_add(600, self.take_shots)
        return False

    # ------------------------------------------------------------------ pages
    def title(self):
        return "%s — DP capability" % self.v.name.decode()

    def draw_plot(self, area, cr, W, H):
        if not self.results:
            return
        show = ("both", "intact", "wcsf")[self.show.get_selected()]
        polar_plot(cr, W, H, self.results, show, self.unit.get_selected() == 1, self.title())

    def fill_table(self):
        lines = []
        cols = []
        for k, r in self.results.items():
            for c in r.cases:
                cols.append((k, c))
        head = "from " + "".join("%-14s" % (NAMES[k].split(" (")[0][:13]) for k, c in cols)
        head2 = "     " + "".join("%-14s" % c.label[:13] for k, c in cols)
        lines += [head, head2, "-" * len(head)]
        for h in range(E.NHEAD):
            s = "%3d  " % (h * 10)
            for k, c in cols:
                n = c.nums[h] if c.nums else None
                s += ("%2s %5.1f       " % ("" if n is None else n, c.winds[h]))[:14]
            lines.append(s)
        lines.append("")
        lines += ["%s: %s" % (NAMES[k], r.summary) for k, r in self.results.items()]
        lines.append("winds in m/s (DNV 10-min mean, ABS 1-min mean); number = the Beaufort step held")
        self.table.get_buffer().set_text("\n".join(lines))

    def live_state(self):
        abs_ = self.live_method.get_selected() == 1
        mask = 0
        for i, c in enumerate(self.thr_checks):
            if c.get_active():
                mask |= 1 << i
        return abs_, mask

    def live_vessel_env(self, wind):
        abs_, mask = self.live_state()
        if abs_:
            cur = self.abs_cur.get_value() * M.KN
            v, _ = M.abs_thrust(self.v, cur) if self.abs_thr.get_active() else (self.v, [])
            table = [(r[2], r[0], r[1], cur) for r in M.ABS_TABLE1]
            return v, M.interp_env(table, wind, cur), 1 + self.abs_margin.get_value() / 100, mask
        return self.v, E.env_wind(wind), 1.25, mask

    def live_update(self):
        if not hasattr(self, "v"):
            return
        frm, wind = self.live_dir.get_value(), self.live_wind.get_value()
        v, env, fac, mask = self.live_vessel_env(wind)
        F = M.weather_force(v, self.ex if self.live_method.get_selected() == 1 else None, env, frm, fac)
        t0 = time.perf_counter()
        self.live_alloc = E.alloc(v, mask, (-F[0], -F[1], -F[2]), self.live_alloc)
        us = (time.perf_counter() - t0) * 1e6
        self.live_v = v
        a = self.live_alloc
        state = "HELD" if a.ok else ("NOT HELD: power" if not a.power_ok else "NOT HELD: %.0f %% short" % (a.res * 100))
        self.live_label.set_text("Hs %.1f m  Tp %.1f s  current %.2f m/s\nweather force x%.2f: Fx %.0f kN  Fy %.0f kN  "
                                 "Mz %.0f kNm\n%s   (%d iterations, %.0f µs)"
                                 % (env[1], env[2], env[3], fac, F[0], F[1], F[2], state, a.iters, us))
        self.live_area.queue_draw()

    def live_limit(self, *_):
        frm = self.live_dir.get_value()
        lo, hi = 0.0, 40.0
        for _ in range(16):
            mid = (lo + hi) / 2
            v, env, fac, mask = self.live_vessel_env(mid)
            F = M.weather_force(v, self.ex if self.live_method.get_selected() == 1 else None, env, frm, fac)
            if E.alloc(v, mask, (-F[0], -F[1], -F[2])).ok:
                lo = mid
            else:
                hi = mid
        self.live_wind.set_value(round(lo, 1) - 0.05 if lo > 0.1 else 0)

    def draw_live(self, area, cr, W, H):
        cr.set_source_rgb(1, 1, 1)
        cr.paint()
        if not hasattr(self, "live_v"):
            return
        v, a = self.live_v, self.live_alloc
        cx, cy = W * 0.28, H * 0.5
        sc = min(H * 0.75 / v.lpp, W * 0.25 / v.b)
        L, B = v.lpp / 2, v.b / 2
        pts = [(L, 0), (L * 0.45, B), (-L, B * 0.92), (-L, -B * 0.92), (L * 0.45, -B), (L, 0)]
        cr.set_source_rgb(0.2, 0.2, 0.2)
        cr.set_line_width(1.5)
        for i, (x, y) in enumerate(pts):
            (cr.move_to if i == 0 else cr.line_to)(cx + y * sc, cy - x * sc)
        cr.stroke()
        frm = math.radians(self.live_dir.get_value())
        r0, r1 = min(W, H) * 0.47, min(W, H) * 0.40
        cr.set_source_rgb(0.1, 0.3, 0.8)
        cr.set_line_width(3)
        x0, y0 = cx + r0 * math.sin(frm), cy - r0 * math.cos(frm)
        x1, y1 = cx + r1 * math.sin(frm), cy - r1 * math.cos(frm)
        cr.move_to(x0, y0)
        cr.line_to(x1, y1)
        cr.stroke()
        self._head(cr, x0, y0, x1, y1, 12)
        maxT = max(v.thr[i].tf for i in range(v.nthr))
        bx = W * 0.52
        cr.set_font_size(12)
        for i in range(v.nthr):
            t = v.thr[i]
            x, y = cx + t.y * sc, cy - t.x * sc
            on = bool(self.thr_checks[i].get_active())
            cr.set_source_rgb(0.1, 0.1, 0.1) if on else cr.set_source_rgb(0.7, 0.7, 0.7)
            cr.arc(x, y, 5, 0, 2 * math.pi)
            cr.fill() if on else cr.stroke()
            fx, fy = a.f[i][0], a.f[i][1]
            fm = math.hypot(fx, fy)
            if on and fm > 0.5:
                l = fm / maxT * min(W, H) * 0.18
                cr.set_source_rgb(0.85, 0.2, 0.1)
                cr.set_line_width(2.5)
                ex_, ey = x + fy / fm * l, y - fx / fm * l
                cr.move_to(x, y)
                cr.line_to(ex_, ey)
                cr.stroke()
                self._head(cr, x, y, ex_, ey, 8)
            # the bar
            yb = 40 + i * 30
            cr.set_source_rgb(0, 0, 0)
            cr.move_to(bx, yb + 12)
            cr.show_text("%d %s" % (i + 1, t.name.decode()))
            cr.rectangle(bx + 70, yb, 150, 16)
            cr.set_line_width(1)
            cr.stroke()
            u = a.use[i] if on else 0
            cr.set_source_rgb(0.85, 0.2, 0.1) if u > 0.98 else cr.set_source_rgb(0.2, 0.5, 0.2)
            cr.rectangle(bx + 72, yb + 2, 146 * u, 12)
            cr.fill()
            cr.set_source_rgb(0, 0, 0)
            cr.move_to(bx + 228, yb + 12)
            cr.show_text("%3.0f %%  %4.0f kN" % (u * 100, fm) if on else "off")

    @staticmethod
    def _head(cr, x0, y0, x1, y1, s):
        a = math.atan2(y1 - y0, x1 - x0)
        for d in (2.6, -2.6):
            cr.move_to(x1, y1)
            cr.line_to(x1 + s * math.cos(a + d), y1 + s * math.sin(a + d))
        cr.stroke()

    def on_map(self, *_):
        self.map = E.map_l1(self.v, E.case_mask(self.v, 0))
        self.map_area.queue_draw()

    def draw_map(self, area, cr, W, H):
        cr.set_source_rgb(1, 1, 1)
        cr.paint()
        if not self.map:
            return
        x0, y0, w, h = 60, 30, W - 180, H - 80
        cw, ch = w / E.NHEAD, h / E.NCUR
        for hd in range(E.NHEAD):
            for c in range(E.NCUR):
                t = min(self.map[hd][c] / 35.0, 1.0)
                cr.set_source_rgb(1 - 0.85 * t, 0.15 + 0.6 * t, 0.15 + 0.2 * t)   # red weak .. green strong
                cr.rectangle(x0 + hd * cw, y0 + (E.NCUR - 1 - c) * ch, cw + 0.5, ch + 0.5)
                cr.fill()
        cr.set_source_rgb(0, 0, 0)
        cr.set_font_size(12)
        for d in range(0, 360, 30):
            cr.move_to(x0 + d / 10 * cw, y0 + h + 16)
            cr.show_text("%d" % d)
        for c in range(0, E.NCUR, 4):
            cr.move_to(10, y0 + (E.NCUR - 1 - c) * ch + ch * 0.7)
            cr.show_text("%.1f" % (c * 2.0 / (E.NCUR - 1)))
        cr.move_to(x0, 20)
        cr.show_text("DNV L1 limiting wind (m/s) by direction (deg) and current (m/s, left), intact")
        for i, wv in enumerate(range(0, 36, 5)):
            t = wv / 35
            cr.set_source_rgb(1 - 0.85 * t, 0.15 + 0.6 * t, 0.15 + 0.2 * t)
            cr.rectangle(W - 100, y0 + i * 24, 20, 20)
            cr.fill()
            cr.set_source_rgb(0, 0, 0)
            cr.move_to(W - 74, y0 + i * 24 + 15)
            cr.show_text("%d m/s" % wv)

    # ------------------------------------------------------------------ export
    def save_png(self, path, W=1400, H=900):
        surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
        cr = cairo.Context(surf)
        polar_plot(cr, W, H, self.results, ("both", "intact", "wcsf")[self.show.get_selected()],
                   self.unit.get_selected() == 1, self.title())
        surf.write_to_png(path)

    def save_csv(self, path):
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            cols = [(k, c) for k, r in self.results.items() for c in r.cases]
            w.writerow(["from_deg"] + ["%s: %s %s" % (NAMES[k], c.label, x) for k, c in cols for x in ("number", "wind")])
            for h in range(E.NHEAD):
                row = [h * 10]
                for k, c in cols:
                    row += [c.nums[h] if c.nums else "", "%.2f" % c.winds[h]]
                w.writerow(row)
            for k, r in self.results.items():
                w.writerow([NAMES[k], r.summary] + r.notes)

    def _save_dialog(self, name, fn):
        d = Gtk.FileDialog(title="Save", initial_name=name)

        def done(dlg, res):
            try:
                fn(dlg.save_finish(res).get_path())
            except GLib.Error:
                pass
        d.save(self.win, None, done)

    def on_png(self, *_):
        self._save_dialog("dpcap47-plot.png", self.save_png)

    def on_csv(self, *_):
        self._save_dialog("dpcap47.csv", self.save_csv)

    # ------------------------------------------------------------------ --shot DIR: the pages as PNG (docs)
    def take_shots(self):
        os.makedirs(self.shot, exist_ok=True)
        self.save_png(os.path.join(self.shot, "plot-export.png"))
        self.save_csv(os.path.join(self.shot, "results.csv"))
        self.on_map()
        pages = ["plot", "table", "live", "map", "vessel"]
        self._shot_i = 0

        def next_page():
            if self._shot_i >= len(pages):
                self.quit()
                return False
            self.nb.set_current_page(self._shot_i)
            GLib.timeout_add(500, grab)
            return False

        def grab():
            os.system("import -window root %s" % os.path.join(self.shot, "gtk-%s.png" % pages[self._shot_i]))
            self._shot_i += 1
            GLib.idle_add(next_page)
            return False
        next_page()
        return False


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opt = lambda k: next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--%s=" % k)), None)
    meth = opt("methods")                       # e.g. --methods=l1,l2,site,abs
    App(args[0] if args else None, opt("shot"), meth.split(",") if meth else None).run([sys.argv[0]])


if __name__ == "__main__":
    main()
