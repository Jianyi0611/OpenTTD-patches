# Carried vehicle tile hash consistency fix (2026-10-17)

The problem save produced repeated `vehicle tile hash mismatch` reports after
joining a multiplayer server. Detailed sampling confirmed that every failing
vehicle had `RV_TRANSPORT_CARRIED` set and an intentionally absent tile hash.
Carried vehicles retain their pickup location but must not occupy the track.
The validator did not account for that distinction.

A shared membership rule now governs both tile hash updates and validation.
It excludes carried and virtual vehicles, invalid locations and the freeform
map-edge sentinel. Ordinary position refreshes can no longer reinsert a carried
vehicle as a phantom obstacle. The checker still rejects a carried vehicle
which is incorrectly indexed, and an ordinary vehicle which is absent.

A related load recovery path cleared stale carried flags from following parts
without restoring their track occupancy. It now updates their position index
when bringing them back onto the network. Normal boarding, unloading and failed
unloading paths were checked for flag/index ordering.

Two regression cases reproduce the previous failures and verify boarding,
refreshing while carried, ordinary carrier occupancy, unloading, and restoring
following parts with stale saved transport state. Both failed before their
respective fixes and pass afterward. The full suite passes 249 cases and 4665
assertions, and the Windows x64 Release application builds successfully.

The original problem save is also used with a dedicated server and two clients.
An isolated diagnostic build checks every vehicle index every 16 simulation
ticks; the final source contains no temporary sampling or local log paths.
The diagnostic run sampled 668 complete vehicle-index scans across server and
clients over approximately 4096 server simulation ticks, with zero invalid
entries. A separate Windows native-driver test uses the final executable,
without temporary sampling. Both clients remained active with frame lag 1
until normal server shutdown during the approximately two-minute test; no
new consistency reports or desync disconnects were observed. The final
headless-driver attempt remained pre-active and is excluded from passing
runtime coverage.
Detailed runtime results are retained outside the repository alongside the
before/after regression logs. The diagnostic message keeps tile, indexed tile
and transport flags so future genuine failures can be investigated.

No save format or feature-version change is needed. Existing PXP checkpoints,
nested schedule returns and the merged upstream px-patch work are retained.
Use matching updated builds on the server and all clients.
