"""engine.py - DPCAP47's C engine (src/dpcap.c, built as build/libdpcap.so by `make lib`) from Python, by ctypes.

The same C file runs on the DM42n, so DNV level 1 here gives the calculator's numbers. The structures below copy
src/dpcap.h; dp_sizeof() checks that the copy and the C agree.
Copyright (C) 2026 Victor Valdes - GPL-3.0-or-later.
"""
import ctypes as C
import math
import os

MAX_THR, MAX_GRP, NHEAD, NBF, NCUR = 12, 6, 36, 12, 17
MAX_CASE = MAX_GRP + 1
T_TUN, T_AZI, T_AZD, T_PROP = range(4)
TYPES = ("TUN", "AZI", "AZD", "PROP")


class Thruster(C.Structure):
    _fields_ = [("name", C.c_char * 8), ("type", C.c_int), ("x", C.c_float), ("y", C.c_float), ("kw", C.c_float),
                ("kn", C.c_float), ("grp", C.c_int), ("fz_a", C.c_float), ("fz_b", C.c_float),
                ("rudder", C.c_int), ("cpp", C.c_int), ("duct", C.c_int), ("rounded", C.c_int), ("d", C.c_float),
                ("tf", C.c_float), ("tr", C.c_float), ("cl", C.c_float)]


class Vessel(C.Structure):
    _fields_ = [("name", C.c_char * 32), ("lpp", C.c_float), ("b", C.c_float), ("t", C.c_float),
                ("afw", C.c_float), ("alw", C.c_float), ("xlw", C.c_float), ("alc", C.c_float), ("xlc", C.c_float),
                ("nthr", C.c_int), ("thr", Thruster * MAX_THR), ("ngrp", C.c_int), ("gen", C.c_float * (MAX_GRP + 1))]


class Env(C.Structure):
    _fields_ = [("wind", C.c_float), ("hs", C.c_float), ("tp", C.c_float), ("cur", C.c_float)]


class Alloc(C.Structure):
    _fields_ = [("f", (C.c_float * 2) * MAX_THR), ("res", C.c_float), ("use", C.c_float * MAX_THR), ("ok", C.c_int),
                ("power_ok", C.c_int), ("iters", C.c_int)]


class Result(C.Structure):
    _fields_ = [("ncase", C.c_int), ("num", (C.c_byte * NHEAD) * MAX_CASE), ("wind", (C.c_float * NHEAD) * MAX_CASE),
                ("wnum", C.c_byte * NHEAD), ("wwind", C.c_float * NHEAD), ("A", C.c_int), ("B", C.c_int),
                ("C", C.c_int), ("D", C.c_int), ("solves", C.c_uint32), ("iters", C.c_uint32)]


def _load():
    here = os.path.dirname(os.path.abspath(__file__))
    for p in (os.path.join(here, "..", "..", "build", "libdpcap.so"), os.path.join(here, "libdpcap.so")):
        if os.path.exists(p):
            lib = C.CDLL(os.path.abspath(p))
            break
    else:
        raise OSError("build/libdpcap.so not found: run `make lib` in the repo first")
    lib.dp_parse.argtypes = [C.POINTER(Vessel), C.c_char_p, C.c_char_p, C.c_int]
    lib.dp_prepare.argtypes = [C.POINTER(Vessel)]
    lib.dp_force.argtypes = [C.POINTER(Vessel), C.POINTER(Env), C.c_float, C.c_float * 3]
    lib.dp_alloc.argtypes = [C.POINTER(Vessel), C.c_uint32, C.c_float * 3, C.POINTER(Alloc), C.c_int]
    lib.dp_case_mask.argtypes = [C.POINTER(Vessel), C.c_int]
    lib.dp_case_mask.restype = C.c_uint32
    lib.dp_held.argtypes = [C.POINTER(Vessel), C.c_uint32, C.POINTER(Env), C.c_float, C.POINTER(Alloc)]
    lib.dp_analyse.argtypes = [C.POINTER(Vessel), C.POINTER(Result), C.c_void_p]
    lib.dp_map.argtypes = [C.POINTER(Vessel), C.c_uint32, C.POINTER(C.c_float), C.c_void_p]
    lib.dp_limit_wind.argtypes = [C.POINTER(Vessel), C.c_uint32, C.c_float, C.c_float, C.c_int, C.POINTER(Alloc)]
    lib.dp_limit_wind.restype = C.c_float
    lib.dp_env_wind.argtypes = [C.c_float]
    lib.dp_env_wind.restype = Env
    for i, s in enumerate((Thruster, Vessel, Alloc, Result, Env)):
        if lib.dp_sizeof(i) != C.sizeof(s):
            raise OSError("libdpcap.so and engine.py disagree on %s (%d vs %d bytes): rebuild with `make lib`"
                          % (s.__name__, lib.dp_sizeof(i), C.sizeof(s)))
    return lib


lib = _load()
dp_dyn = C.c_float.in_dll(lib, "dp_dyn")
dp_solves = C.c_uint32.in_dll(lib, "dp_solves")


def table21():
    """DNV-ST-0111 Table 2-1 as the C has it: [(wind, hs, tp, cur)] for Beaufort 0..11."""
    arr = (Env * NBF).in_dll(lib, "DP_TABLE")
    return [(e.wind, e.hs, e.tp, e.cur) for e in arr]


def example_text():
    """The example vessel file built into the program."""
    return C.cast(C.addressof(C.c_char.in_dll(lib, "DP_EXAMPLE")), C.c_char_p).value.decode()


def parse(text):
    """A vessel file's text -> Vessel (the C parser: the same as the calculator's). Raises ValueError."""
    v = Vessel()
    err = C.create_string_buffer(80)
    if lib.dp_parse(C.byref(v), text.encode(), err, 80):
        raise ValueError(err.value.decode())
    return v


def copy(v):
    w = Vessel()
    C.memmove(C.byref(w), C.byref(v), C.sizeof(Vessel))
    return w


def thrusters(v):
    return [v.thr[i] for i in range(v.nthr)]


def case_mask(v, c):
    return lib.dp_case_mask(C.byref(v), c)


def force_l1(v, env, frm):
    """The level 1 (provisional coefficient) force of the weather, times dp_dyn: (Fx, Fy, Mz)."""
    tau = (C.c_float * 3)()
    e = Env(*env)
    lib.dp_force(C.byref(v), C.byref(e), frm, tau)
    return tau[0], tau[1], tau[2]


def alloc(v, mask, tau, a=None, maxit=600):
    """Allocate tau (what the thrusters must give). Returns the Alloc (ok, res, f, use)."""
    if a is None:
        a = Alloc()
    t = (C.c_float * 3)(*tau)
    lib.dp_alloc(C.byref(v), mask, t, C.byref(a), maxit)
    return a


def analyse_l1(v):
    r = Result()
    lib.dp_analyse(C.byref(v), C.byref(r), None)
    return r


def map_l1(v, mask):
    m = (C.c_float * (NHEAD * NCUR))()
    lib.dp_map(C.byref(v), mask, m, None)
    return [[m[h * NCUR + c] for c in range(NCUR)] for h in range(NHEAD)]


def held_l1(v, mask, env, frm, a=None):
    if a is None:
        a = Alloc()
    e = Env(*env)
    lib.dp_held(C.byref(v), mask, C.byref(e), frm, C.byref(a))
    return a


def env_wind(w):
    e = lib.dp_env_wind(w)
    return (e.wind, e.hs, e.tp, e.cur)


def limit_l1(v, mask, frm, a=None):
    if a is None:
        a = Alloc()
    return lib.dp_limit_wind(C.byref(v), mask, frm, -1.0, -1, C.byref(a))


def deg(x):
    return math.radians(x)
