# DPCAP47: working rules

- Work on a new branch; Victor creates and merges the PRs. Never force-push or rewrite history.
- Commit author: "Victor Valdes" <valdesvj@users.noreply.github.com>.
- The same base as HP47, ZX47, Chess 47, NAV47, TERM47 and GB47 (dmcp/, tools/pg5tool.py, test/sim): keep them in step.
- DMCP5: 8 KB of static RAM. Big buffers go on the heap (the results, the map, the printer's bitmap); check .bss.
- The engine (src/dpcap.c) is plain C with no DMCP: the same file runs in the PC tests. Keep it so.
- DNV-ST-0111 (Dec 2021) level 1 is implemented section by section (the table at the top of dpcap.c). Anything that
  is not the standard's own (a default, a typical kN/kW without a diameter) is flagged on screen and in the README.
- Victor's copy of DNV-ST-0111 is licensed to him only: never commit it or quote long passages from it.
- No real vessel's data in the repo: the example vessels are made up.
- The PC program (tools/dpcap47_gtk.py, tools/dpcap) uses the C engine through build/libdpcap.so: change a structure
  in dpcap.h, change engine.py the same way (dp_sizeof checks it). Never put the allocation in Python.
- make test, make lpcheck, make pccheck, make and make shots before committing; look at every screen. The simulator's timings mean
  nothing; the calculator's are measured on the DM42n.
- release/DPCAP47.pg5 is built with plain make.
- Ask before anything that can't be undone. Pull requests only for dpcap47.pg5.
