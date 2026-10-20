# Runtime stability and multiplayer audit

## Moving-train order modification

The reported Linux crash context is ModifyOrder with payload `0, 1731, 0, 25, 1, 0`. Flag 25 enables station decoupling. A separate command-level reproduction with a moving train confirmed a heap-use-after-free: inserting the following decouple marker reallocates the OrderList vector, after which CmdModifyOrder reads the old station pointer while synchronising active orders. AddressSanitizer identified GetLoadType in the current-order update and the freeing emplace in OrderList::InsertOrderAt. Reserving spare vector capacity avoids this invalid read. This establishes the source defect, but the screenshot alone cannot symbolicate the original Linux instruction address.

The fix preflights marker insertion before modifying station flags, propagates insertion failure and reacquires the order after structural changes. Disabling a missing marker no longer deletes an unrelated successor or invokes out-of-range vehicle decloning. Boolean data is validated. Standalone schedule edits also synchronise active station-order copies. Regression tests cover forced reallocation, two shared moving trains, vehicle and standalone edits, the order-count limit, invalid data and missing markers. No save fields, command IDs or wire parameters were added for this fix.

## Custom airports

- Validate tiles, tracks, surface types, rotations, airport layouts and infrastructure graphics before table access or map mutation. Unsupported edit operations return command errors instead of reaching assertions.
- Check hangar-pool capacity before creating a first hangar and enforce the existing airport height when extending it.
- Rebuild each affected station once in stable StationID order instead of once per changed tile.
- Reconnect legacy duplicate hangar records using the lowest matching DepotID on both an existing server and a joining client. Preserve persistent depot identities and orders.
- Reject duplicate raw blueprint airport offsets, invalid graphics and incomplete runways before terrain changes. Crossing runways require complete coverage of both branches; the validation walk is bounded by payload size.

## Verification

Release build and all 275 tests / 5951 assertions passed. An additional build instrumenting order_cmd.cpp with AddressSanitizer passed 277 tests / 5970 assertions, including the two independent investigation tests. This is targeted instrumentation, not a claim that every translation unit was sanitised.

A local server with two native clients ran a save containing five airports (two custom) and one aircraft. Matching-tick diagnostics compared airport tiles and infrastructure caches, hangar identities, aircraft state and paths, train positions/orders/claims and simulation random seeds. The first client had 42 matching samples and the second 36; all 78 comparisons matched, with no invalid vehicle tile hashes. Both clients remained connected and the server exited normally. This diagnostic run preceded the final order-modification fix and malformed-blueprint checks; a clean final-binary network smoke test is recorded separately with the package.

Known bounds: this does not prove absence of all multiplayer faults, cover every NewGRF or airport geometry, or replace symbolication of the original Linux crash. Simulation-changing fixes and the new mandatory speed-filter save feature require all peers to use the same build. Existing older saves load; older executables reject saves containing the new speed criteria rather than ignoring them.
