# AI Assistance Policy

## How this project uses AI

This project was **developed with the help of AI** — Claude (Anthropic) — as a cooperation. AI assisted with brainstorming, code generation, and documentation, but I reviewed all output, validated results against reference implementations, and take full responsibility for the work and any mistakes.

## Responsible use

- ✅ **I checked the results** — The thrust allocation is checked against an exact linear programme (scipy) for every direction, failure case and Beaufort number (`make lpcheck`), and the engine's own checks run on the PC (`make test`)
- ✅ **I maintained domain expertise** — I guide the AI with the design decisions: DP operations (I am a DP Master on diving support vessels), what the plot and the numbers must show, the keys and the screens
- ✅ **I take full responsibility** — The work is mine; any errors are mine to fix
- ✅ **I'm transparent** — This file documents how AI was used

## Testing & Validation

- The engine's checks: Table 2-1, the efficiencies, the force balance, symmetry, power limits, convergence (`make test`)
- The allocation against an exact LP (`make lpcheck`)
- Every screen checked in the PC simulator (`make shots`) before a commit
- To be tested on real DM42n hardware, and against DNV's own level 1 tool

## License

This does not affect the GPL-3.0-or-later license. All code remains open-source and fully accessible.

---

*For more details, see `LICENSE` and the README.*
