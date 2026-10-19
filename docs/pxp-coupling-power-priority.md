# Speed-first locomotive selection with optional power priority

Ordinary target selection retains the existing bounded speed/distance score. Before the first exclusive claim, extra locomotive arbitration runs only when the caller's coupling order enables at least one speed bound (Own maximum or Manual). Competitors must also enable a speed bound on their own intended coupling order. Both bounds disabled preserves ordinary claims and skips the fleet scan. Existing valid claims on either side remain exclusive.

Rank ready compatible locomotives for the chosen waiting consist by:

1. Smallest absolute maximum-speed difference from that consist.
2. At equal differences, prefer a locomotive that does not exceed its maximum speed.
3. At equal speed fit, higher total consist power (`gcache.cached_power`, including multiple engines).
4. Actual route cost, then stable vehicle ID.

Power can never compensate for worse speed fit. A fast powerful locomotive therefore yields to a ready reachable slower matching locomotive, but keeps a normal coupling opportunity if no better candidate passes its own criteria and bounded route search. A live claim is never stolen when a better locomotive later becomes available.

A contender must be stationary, powered, nonvirtual, not carried, not manually stopped, not crashed, not in a depot, and not undergoing breakdown or reversal. An early-checkpoint contender must have completed stop preparation and be waiting at its configured checkpoint. The caller can arbitrate during the actual arrival event before stopping. A completed reverse checkpoint remains eligible with its pending flag even if passing the whole waypoint advanced the order index. Ordinary loading and intermediate orders cannot win priority.

The winner receives the existing claim directly and retains its current order and nested-plan return stack. Reachability queries pass explicit coupling criteria and a single target; they never temporarily change current orders, reserve tracks, consume random numbers, use global path caches or modify other claims.

## Performance and determinism

- Disabled speed checks exit before the additional fleet scan.
- Candidate rank, approximate distance and criteria pointer are computed once per arbitration instead of repeatedly during sorting and route queries.
- Partial sorting orders only the first 16 candidates. Compared with sorting all eligible candidates this changes sorting complexity from O(n log n) to O(n log 16), while preserving the same deterministic prefix.
- At most 16 competitor route probes run, each limited to 1024 search nodes or the smaller configured limit. An equal-priority comparison can require one additional query of the original route. Budget exhaustion preserves the original caller.
- No cross-tick caches, new random inputs, save fields, command payloads or save-format changes. Peers must use the same executable because selection behaviour changed.

These reduce work in coupling arbitration; they are not a measured claim of an overall frame-rate gain on every map. Custom airports and other simulation features remain unchanged.

## Validation

- Windows x64 Release build passed.
- Full suite: 286 test cases / 6894 assertions passed.
- Targeted train_cmd.cpp AddressSanitizer: 10 coupling-priority cases / 637 assertions passed.
- Real-rail fixtures cover disabled caller and competitor checks, either bound and source mode, speed before power, equal-gap preference, unreachable/stopped/rejecting matching locomotives, ordinary fallback, live claims, route/ID ties, search budgets, checkpoint arrival and reversal. They check random state, reservations, vehicle tile hashes and retained active orders/stacks.

Diagnostic hooks stay outside the repository and player package. This work is local only; no GitHub upload is authorized.

- Local diagnostic server plus two native clients: 42 and 36 matching same-tick samples; no differences or invalid vehicle hashes. Compared vehicle positions, speed/power, orders/stacks/claims, aircraft and two custom airports, plus random seeds. Both clients stayed active until controlled shutdown; server exit code 0.
- This save did not trigger a new eligible priority contest (diagnostic counters remained zero). Contest behaviour is covered by real-rail regression scenarios rather than this multiplayer save. This does not establish absence of every possible desync.
