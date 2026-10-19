# Same-speed locomotive power priority

The existing speed/distance score first chooses a waiting consist. Before its first exclusive claim, ready unclaimed locomotives with the same maximum speed compete for that same waiting consist. Prefer total consist power (`gcache.cached_power`, including multiple engines), then actual route cost, then stable vehicle ID. A live claim on either locomotive or waiting consist is never reassigned.

A contender must be stationary, powered, nonvirtual, not carried, not manually stopped, not crashed, not in a depot, and not undergoing breakdown or reversal. Its coupling criteria must accept the waiting consist. An early-checkpoint contender must have completed its stop preparation and actually be waiting at its configured checkpoint. The original caller may also arbitrate during the actual arrival event before its speed has been reduced to zero. A completed reverse checkpoint remains eligible while its pending flag is active even if passing the whole waypoint advanced the order index. A train still loading or performing intermediate orders cannot win on power alone.

The winner is granted the existing claim directly. The original caller's normal claim check then makes it wait or look for another target. This avoids two locomotives leaving for one consist and avoids depending on which locomotive ticks first. The winner retains its current station/waypoint order and its nested-plan return stack.

Reachability probes pass an explicit copy of the intended coupling criteria and one permitted waiting target through the coupling pathfinder; they never temporarily change a locomotive's current order. Probes do not reserve tracks, consume random numbers, use global path caches or modify other claims. For a fixed single destination the speed bonus is disabled because it cannot change the destination ranking.

To bound additional work, inspect at most 16 eligible competitors (power, approximate distance, ID order), with at most 1024 search nodes per query and any lower configured limit respected. An equal-power comparison can require one additional query of the original route. If no superior candidate can be proven reachable within the budget, preserve the original caller. Stronger but unavailable locomotives therefore cannot make the waiting consist wait indefinitely for them. The existing ordinary coupling search and its fallback keep their original limits.

No save fields, network payloads, commands or random state were added. Multiplayer peers must nevertheless use the same executable because target-selection behaviour changed.

## Validation

- Full release build and 283 test cases / 6702 assertions passed.
- Targeted AddressSanitizer instrumentation of train_cmd.cpp passed all seven new power-priority test cases / 445 assertions.
- Real-rail fixtures cover higher power, unreachable candidates, node/fleet budgets, stopped/crashed/broken/reversing/moving/unpowered/virtual/carried locomotives, invalid positions, empty rail types, speed mismatch, rejecting criteria, unfinished loading, existing claims on either side, equal-power route/ID ties and ready versus unready early checkpoints, arrival before stopping and reverse-checkpoint index advance. They check retained active orders, unchanged plan stacks, random seeds, tile hashes and reservations.
- The initial local server and two native clients ran a save with 531 train vehicle objects, two aircraft vehicle objects (including auxiliary objects), and two custom airports. The clients respectively matched 42 and 36 same-tick samples of vehicle positions, cached power/speed, order lists and return stacks, claims, aircraft state, airport tiles/hangar identity and random state. No mismatches or invalid vehicle tile hashes occurred. Both clients remained running and the server exited normally.
- This save did not produce a new eligible same-speed power contest during the network run (diagnostic contest counters stayed at zero). The new contest branches are covered by the real-rail regression cases; the multiplayer run verifies the live inputs and surrounding simulation, not exhaustive contest coverage across all maps or NewGRFs.

Diagnostic source copies, hooks and executables live outside the repository and are excluded from the player package. This change is local only; no GitHub push was performed.

The final checkpoint-boundary version is also checked with a second diagnostic multiplayer run and a clean local release build; final run details are included with the local package.
