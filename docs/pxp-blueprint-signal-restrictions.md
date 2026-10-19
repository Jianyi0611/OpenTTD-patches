# Blueprint signal routing restrictions

Copying a rail signal now captures an independent snapshot of its routing restriction instructions and text labels, together with the existing signal type, direction and NewGRF style. The snapshot survives deleting or editing the source signal and travels inside the ordinary blueprint paste command. No client-side clipboard, source program ID or source tile is consulted during command execution.

Every newly pasted restriction is independent, including when source signals shared a program. Existing destination restrictions are preserved. Ordinary signals without a restriction keep their previous behaviour.

Tile operands referring inside the copied area are stored with relative coordinates. Shrinking, rotating, reflecting and pasting relocate them with the blueprint. Cardinal entry/target direction predicates rotate too; signal-relative front/back tests retain their meaning. References to copied stations and waypoints are updated to newly built station IDs. External tile, station, depot, group, slot and counter references retain their existing meaning in the current game; this does not clone those external resources. Non-owned private slot/counter references are cleared using the same policy as the standard restriction-copy command; public and owned references remain usable.

Before building, paste validates program structure, labels, relocation operands and bounds, and checks program pool capacity. Relocations cannot overwrite arbitrary instruction words. Malformed/truncated snapshots and oversized snapshot list counts are rejected. Probe mode does not create a restriction or change an existing program. Normal signal removal also removes its copied restriction through the existing mapping lifecycle.

The blueprint clipboard/set/autosave format is now version 6. Versions 3, 4 and 5 still load with missing fields defaulted. Existing game save format is unchanged: pasted programs use the existing routing restriction save/load code. All multiplayer participants need the same executable to interpret new blueprint command payloads.

## Verification

- Windows x64 Release build passed.
- Complete suite: 293 cases / 7013 assertions passed.
- Regression tests cover deletion of the source before paste, command payload transport, real signal construction, relocated operands, independent second paste, labels, all eight rotations/reflections, malformed programs, relocation misuse, old v5 import, dry-run behaviour, preservation of existing restrictions, and owned/public/private slots.


- Targeted AddressSanitizer build instruments blueprint.cpp and tracerestrict.cpp: all 41 blueprint cases / 717 assertions passed.
- Local dedicated server and two native clients: 42 and 36 matching same-tick samples, no state differences or invalid vehicle tile hashes. The server posted a format-6 blueprint paste with a restriction label through the normal network command path. Compared program words, labels, mapping references, vehicle state, orders, claims, random state and two custom airports. Both clients remained active until controlled shutdown; server exited with code 0. This is bounded regression coverage, not proof against every possible desync.
