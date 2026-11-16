# DPCAP47 — DP capability on the SwissMicros DM42n

**DPCAP47.pg5** is a stand-alone DMCP5 program written in C for the SwissMicros **DM42n**. It computes a dynamic
positioning vessel's **station-keeping capability** on the calculator itself, following the frame of
**DNV-ST-0111 level 1**:

- the capability plot (the limiting wind all round, intact and after the worst single failure)
- the **DP Capability-L1 (A, B, C, D)** numbers
- a live view where you turn the weather, raise the wind and switch thrusters off, and watch each thruster's use and
  whether she holds
- a map of the limiting wind for every direction and every current from 0 to 2 m/s
- printing the plot to an HP 82240 printer over the infrared port

Part of the "47" family of projects (almanac47, chess47.pg5, nav47.pg5, hp47.pg5, zx47.pg5 …).

![DPCAP47 screens](docs/screens.png)

> **Version 0.1: first build, not yet tested on the calculator.** Everything here runs on the PC (`make test`,
> `make lpcheck`, `make shots`). The DM42n's speed, and printing over IR, are still to be seen.

> **Not for class, contracts or operations.** DPCAP47 follows DNV-ST-0111 level 1's frame, but some of its formulas
> are **provisional** (see [What follows the standard](#what-follows-the-standard)). It is not DNV's software and is
> not approved by DNV. For a real assessment, use DNV's own tool or a DP capability analysis by a competent body.

## Install on the DM42n

1. **Back up the calculator disk first** (connect USB, copy everything).
2. Copy `release/DPCAP47.pg5` to the root of the calculator's USB disk.
   Also keep the current `DM42-x.yy.pg5` (or your C47 `.pg5`) there, so you can go back.
3. Open the DMCP menu: **SETUP → System → Run DMCP**, or press **RESET while holding F1**.
4. Choose **Load program → DPCAP47.pg5**.

At the start DPCAP47 writes `/DPCAP47/EXAMPLE.DPV`, then runs the level 1 analysis of the example vessel. The time it
took is shown under the numbers. To return to Free42 or C47: **EXIT → 4 setup → System**, or use **RESET+F1**.
DPCAP47 needs DMCP5 3.57 or later.

## The pages

| Key | Page | What it shows |
|---|---|---|
| F1 | **PLOT** | The capability plot: the limiting wind (m/s) for weather from each direction, intact (thick line) and the worst single failure (dashed). The A B C D numbers. ENTER computes it again. |
| F2 | **TABLE** | Each direction's capability number and limiting wind: intact, the worst single failure, and each group's failure. UP/DOWN scroll. |
| F3 | **LIVE** | The vessel, the weather's arrow and each thruster's force. UP/DOWN turn the weather 10°. + − change the wind by 1 m/s, × ÷ by 0.1. Keys 1–9 switch a thruster off and on, `.` fails the next redundancy group, 0 restores all. **ENTER finds the limiting wind for this direction, live.** |
| F4 | **MAP** | ENTER computes the limiting wind for 36 directions × 17 currents (0 to 2 m/s): 612 searches. Black means the vessel cannot hold even at Beaufort 0, white means it holds at 11. |
| F5 | **VESL** | The vessel file: the hull, the areas, and each thruster with the thrust left after the losses. |
| F6 | **PRINT** | The numbers, the plot and the table to an HP 82240 over IR (untested on a printer yet). |
| EXIT | menu | Load a vessel file, write the example, setup (time, USB disk, System), about, OFF. |

The numbers, as in DNV-ST-0111 2.2 (Table 2-1 is the Beaufort scale's wind, Hs, Tp and current):

- **A**: the lowest number for weather within 30° of the bow, intact
- **B**: the lowest all round, intact
- **C**: the lowest within 30° of the bow, worst single failure
- **D**: the lowest all round, worst single failure

The worst single failure is the loss of each redundancy group in turn (the lowest result a direction).

![The printout](docs/print.png)

## Your own vessel: the .DPV file

Plain text in `/DPCAP47` on the calculator's disk. Write it on the PC and copy it over, then **EXIT → 1** to pick it.
DPCAP47 remembers the file.

```
name=EXAMPLE DSV
lpp=110                 # m
b=22
t=7                     # summer draught
afw=420                 # wind: frontal area, m2
alw=1500                # wind: side area, m2
xlw=5                   # the side area's centre, m forward of midships
alc=770                 # current: underwater side area (default lpp*t)
xlc=0
gen=1,6000              # kW group 1 generates (optional; 10 % is kept for the ship)
# thr=NAME,TYPE,x,y,kW,kN,group,fzFrom,fzTo,OPTS
thr=BT1,TUN,48,0,1500,0,1
thr=BAZ,AZD,40,0,1500,0,3,80,100
thr=MP,PROP,-42,-4,2000,0,1,0,0,RC
```

- **x** is in m forward of midships, **y** in m to starboard.
- **TYPE**: `TUN` tunnel, `AZI` open azimuth, `AZD` ducted azimuth, `PROP` main propeller.
- **kW** is the motor power. **kN**, if not 0, is the nominal bollard thrust and is used instead of the kW.
- **group** is the redundancy group (1–6).
- **fzFrom, fzTo** is a forbidden zone: the thrust directions (where the thrust pushes the vessel, 0° ahead, 90° to
  starboard, clockwise) the azimuth may not use.
- **OPTS**: `R` a rudder behind it, `C` CPP, `D` ducted, `I` a tunnel with a rounded inlet.

`vessels/` has two made-up examples: `EXAMPLE.DPV` (a DSV with tunnels and azimuths) and `PSV.DPV` (a supply vessel
with two CPP propellers and rudders). Neither is a real vessel.

## What follows the standard

**From DNV-ST-0111 level 1:**

- Table 2-1: wind, Hs, Tp and current for each Beaufort number
- Wind, waves and current all from the same direction
- The dynamic factor 1.25 on all three
- 10° directions, with the numbers counted up from the lowest condition
- A B C D as above, with the worst single failure taken over the redundancy groups
- The thruster efficiencies:
  - tunnel inlet 0.93, or 1.07 for a rounded one
  - reversed propellers: FPP 0.9 (0.7 ducted), CPP 0.65 (0.5 ducted)
  - mechanical efficiency 0.93 (tunnel, azimuth), 0.97 (shaft line)
  - 0.9 for miscellaneous losses
- The rudder only behind a propeller going ahead, at 30° at most
- 10 % of a group's power kept for the ship

**PROVISIONAL (not yet the standard's own formulas):**

- The wind, current and wave drift **coefficients**. They are simple drag and drift coefficients for now (`dp_force` in
  `src/dpcap.c`), to be replaced by the formulas of DNV-ST-0111 sections 3.5–3.7.
- The **thrust a kW** of each kind of thruster (`KN_KW`). You can give each thruster's kN instead.
- The rudder's side force (0.35 of the thrust at 30°).

**Not yet:** thruster–thruster interaction and flushing, ventilation, the wave drift's dependence on the period, and
power taken as a limit inside the allocation (it is checked after the allocation).

**How it is computed.** For each direction and each Beaufort step, the thrusters must give the force that holds the
vessel. Each thruster's force must stay inside its own set:

- a tunnel's line
- an azimuth's disc, less its forbidden zone
- a propeller's thrust ahead with its rudder's side force, or astern

The allocation minimises the residual by projected gradient with Nesterov's momentum (FISTA). Where a set is not convex,
the solver tries each combination of its convex pieces. A force is **held** when the residual is under 0.5 % of the
demand and no group uses more power than it has. The limiting wind between the Beaufort steps is then found by
bisection (Hs, Tp and current interpolated in Table 2-1).

`make lpcheck` checks the allocation against an exact linear programme (scipy), for every direction, case and Beaufort
number. On the two example vessels it agrees in all 1580 and 1188 cases (four cases within 1 % of the edge are not
judged).

## Build

```bash
cd ~/opt/dpcap47.pg5
make                          # build/DPCAP47.pg5 (needs arm-none-eabi-gcc)
make test                     # the engine's checks on the PC
make lpcheck                  # the allocation against an exact LP (python3 + scipy)
make table F=vessels/PSV.DPV  # a vessel file's analysis as a table, on the PC
make shots                    # the PC simulator's screens -> build/sim/run/*.png
```

## Credits and license

GPL-3.0-or-later (see `LICENSE` and `NOTICE`). The DMCP5 SDK files are SwissMicros', BSD 3-Clause.
Written with the help of AI, as a cooperation (see [AI_ASSISTANCE.md](AI_ASSISTANCE.md)).
Please don't contact SwissMicros about this program.
