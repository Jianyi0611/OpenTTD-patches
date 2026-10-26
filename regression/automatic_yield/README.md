# Automatic yielding: native waiting regression

Run from an existing Windows Ninja Release build with compile_commands.json and all production objects built:

```powershell
python regression/automatic_yield/run.py --build-dir build-win64 --baseset <OpenGFX-directory> --output <separate-output-directory>
```

The runner links a separate executable against production objects, replacing only the game-loop entry object to construct and observe synthetic maps. It does not overwrite the production executable. Requires the compiler from compile_commands.json and Python 3.8+. This runner currently supports the Windows/Ninja build layout.

Mandatory acceptance and explicit unverified cases are defined in [ACCEPTANCE.zh-CN.md](ACCEPTANCE.zh-CN.md) and the repository `AGENTS.md`.

`far-basic` and `far-two` use real stock vehicles, distant physical station orders (350/400), and a long feeder protected by native PBS safe ends. They assert actual siding entry, exit stop, held reservation, every express's body clearance, restart and slot release. The second case starts a new express while the slow train is already waiting; both cases reload the waiting save.

`layout-add`, `layout-switch`, and `layout-legal-old` change real track through native commands during movement. They check a reconnected candidate without a program edit, a changed inlet with actual use of the other siding, and preservation of a still-legal old branch. Realistic braking rejects editing a moving train's reserved route, which is tested rather than bypassed; that variant changes the empty inlet before acquisition. Conventional braking also exercises disconnection after acquisition and bounded release of the orphan claim. Each variant verifies the complete stop/pass/resume sequence and reload. Signal-direction metadata and body/reservation protection have additional native-command unit tests.

The default matrix has 168 native runs per braking mode (336 for both modes), covering an explicit siding entrance and exit-only registration. Stock core cases use 112 km/h, not exactly 100 km/h; exact-speed cases below require installed real NewGRFs. The specification-changing turnover fixture below is an isolation test only.

Run again with `--braking 0` for conventional braking (default: realistic braking, `--braking 1`). Assertions include actual siding entry, zero speed at its exit, continued zero speed during the express passage, no reserved path crossing the held exit, tail clearance, resumed movement, and resource release. Two-express cases check both mainline passages while the slow train is stopped. Unknown-route cases establish the obligation before introducing a temporary restriction. Front/tail detection cases start the express early enough for registration before the slow train's routing decision.

`normal-free-first` prevents full-lane arrival ranking from vetoing normal admission, and checks the saved obligation while the express temporarily loses its approach reservation. `far-empty-first` has an unreachable first candidate and an available second candidate; searching beyond this area's Clear must not consume the second candidate's budget. `far-multi-outlet` combines a distant station with an irrelevant alternative outlet: positive admission is distinct from proving one unique destination outlet. All three include waiting reloads.

`layout-new-first`, `layout-new-second`, `layout-new-third`, and `layout-new-platform` construct real new parking tracks with native commands. First/platform facilities are completed before initial approach registration; second/third lanes are built while the slow train is moving. Older tracks are removed or rejected by an ordinary routing restriction, so staying on an old lane cannot pass. Every case requires actual use of the new lane, stopping, express passage during the stop, full clearance, resumed movement, and released claims. Existing physical-occupancy/full-siding tests remain separate.

Exact-speed regression is optional only when the installed GRF is unavailable, and must then be reported unverified:

```powershell
python regression/automatic_yield/run.py --build-dir build-win64 --baseset <OpenGFX-directory> --output <exact-output> --newgrf-config <real-vehicles.ini> --modes grf-basic,grf-200,grf-160-250,grf-consecutive
```

The 2026-10-25 run used installed **2cc Trains in NML 3.2**, with its own metadata defaults explicitly specified (file-only configuration does not populate GUI defaults):

```ini
[newgrf]
<absolute-path-to-2ccts.grf> = 4 4 1 1 1 1 1 1 1 1 1 1 1
```

`grf-basic` is 100→160, `grf-200` is 100→200, `grf-160-250` is 160→250, and `grf-consecutive` is 100→160/200/250. `grf-two` additionally supports 100→two 160 engines (also tested with China Set Trains 0.4.1). Vehicles are built and run normally on electrified track; engine specifications, speed, stopping state, and reservations are not patched. Both construction and reload assert the actual design speeds and positive consist power. The three-express case requires three actual mainline passages while the slow train stays stopped; isolated express cases also reject a zero speed on Main. Start times differ by braking model to register the express before the slow train commits its branch, while allowing the slow train to finish braking before passage. These changes do not relax stop/pass/resume assertions. The four 2cc cases produce 16 runs per braking model; `--debug-yapf` includes vehicle catalogue and GRF parameter diagnostics.

`--overtaking-source <saved-production-file>` can rebuild a prior automatic-yield implementation for a failure-before comparison. Its source path and SHA256 are recorded separately; it is never the production executable or proof that all other source objects are historical.

Additional cases cover independent exits, deleting the express, manually cancelling registration, and building an alternative route with a corresponding order change. The latter is not an exhaustive test of every order-edit operation. The different-exit fixture requires actual siding entry to yield the shared approach before the split, followed by continued movement once the routes are independent; merely staying on Main no longer passes. Waiting saves are reloaded for basic, temporary-red, unknown-route, and two-express scenarios.

Stream cases add three expresses arriving from different feeders over time: one before siding entry, one after the slow train stops, and one after the first express has cleared. They use stock 112/160/200/264 km/h engines with a configured 250 km/h detection cap, native commands, and native vehicle movement. `stream-serial` adds successive Main controls on one corridor, which must not be mistaken for parallel escape routes. Each saved obligation must survive the first express's departure and reload until its own conflict clears. `stream-reroute` changes the third express's active order to an independent exit while the second remains pending: the slow train must retain the second obligation. The test verifies the third train actually travels along its new route, rather than only checking its edited order. All three stream scenarios include fresh/reload variants. `--debug-yapf` captures native pathfinder diagnostics for them.

Station cases use native station orders and `CmdChangeTimetable` to set actual dwell times (inserting an order alone does not serialize timetable fields). `station-same` serves two platforms of the same station; `station-merge` joins different feeders after their intermediate stops; `station-different` takes an independent destination after stopping. Shared cases require an actual exit stop overlapping the express's actual loading state, continued waiting until its tail clears, then resumed movement and release. `station-priority` additionally requires diverting an otherwise through-running slow train into a free siding when the conflicting express first stops on another feeder. Each has fresh/reload variants. The latter distinguishes the old mainline wait from proper siding admission, even when native PBS prevented a collision. These use stock engines, without editing their speeds or moving vehicles by hand.

Crossing cases use perpendicular tracks sharing a physical crossing before different exits. The express body must clear the crossing before release; its unrelated downstream clearance signal does not lock the whole station. A separate-detector variant verifies coordination without enrolling the express in the slow train's source slot.

Full-siding cases construct real 100/160/200 km/h trains: the 160 train occupies the sole siding, the 100 train arrives, and the 200 express approaches from another feeder. A distant express permits safe turnover followed by actual waiting of the 100 train; a close express forbids departure of the 160 train until its passage. The 100 engine specification is changed before building the synthetic vehicle, not by teleporting or simulating movement. That fixture-only specification is not serialized by ordinary saves, so these two cases deliberately have no reload variant and their generated maps are not portable examples.

The driver does not prove arbitrary physical networks deadlock-free or guarantee a finite wait under continuous saturation. All movement and track permission still use native signals and PBS. Scenario assertions exercise actual movement; they do not replace the production restriction evaluator or native pathfinder.

results.json and per-case logs record phases and assertion results. Synthetic saves are generated in the output folder. No player save is needed or modified.


## Focused merge/split and usable new-lane regressions (2026-10-26)

`topology-merge2`, `topology-merge3`, `topology-merge3-signals`, `topology-split`, and `topology-rejoin` run stock trains through different physical incoming/outgoing routes. Shared cases require full-body siding parking, a real zero-speed wait overlapping each express passage, no exit reservation, each express tail clearance, restart and released claims. The split case requires simultaneous source membership without a false diversion or stop. The three-entrance case has serial Main markers; its signals variant adds ordinary PBS endpoints inside the siding, delaying the first Exit transaction. The rejoin case serves mandatory non-stop route stations on different branches and waits through their later shared resource.

`station-merge-split` includes actual timetable dwell at both approach platforms, a common throat and a later independent destination branch. It verifies waiting overlaps the express's actual station work before its tail clears the shared path.

`occupied-second`, `occupied-third`, and `occupied-platform` stage unmodified stock holder trains by real movement, then build the additional lane while the arrival is moving. Unlike the earlier routing-denial-only construction tests, all older lanes remain physically occupied. The tested arrival must use the newly built lane, safely park, wait during the express passage, resume and release its own claims. Ordinary explicit staging holds are restricted to setup holders; they do not control the tested arrival. These three cases run fresh/reload and both marker styles.

`far-near-wrong-outlet` has a nearer, genuinely wrong one-way outlet and a farther first outlet reaching the distant station. A two-way dead-end depot can permit a native turnaround, so that geometry is deliberately not treated as an unreachable-outlet regression.

Each topology case exports `topology-portable.sav` only after all required trains have started, before siding entry. Unlike a setup-only snapshot with an express still stopped in its depot, it works in the production executable without future driver commands. Acceptance additionally reloads these snapshots in observation-only mode.

The final matrix count includes the new cases but excludes optional exact-speed NewGRF cases, old separate compound/platform/bridge drivers, and portable observation reloads. Failed intermediate attempts remain in the task evidence; only the final source identity is accepted.
