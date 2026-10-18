# px-patch integration and custom airport synchronization audit

Upstream: pulsexlb/OpenTTD-patches px-patch at
1c98ce363392d8099ae371bd352db48b98c70240. Local baseline: ab5daf0.

The ordinary merge retains the PXP checkpoints, checkpoint save marker,
nested schedule returns, exclusive coupling claims, schedule group UI and
previous carried-vehicle tile hash fix. No other upstream branch is merged.

## Airport fixes

- Airport cache rebuilding no longer destroys and reallocates the persistent
  hangar Depot. Loading reconnects the saved pool item by its airport tile,
  retaining its ID, name and metadata instead of allocating a duplicate.
- Hangar and landing-runway flags are re-derived, including when an airport
  becomes empty; the manual closure flag is preserved.
- Airport-edit notifications resolve aircraft hangar orders in the station ID
  namespace. Removed hangars cannot fall back to an unrelated Depot whose ID
  collides with a valid station. Pre-multitile load conversion no longer
  rewrites canonical station IDs to depot IDs.
- Blueprint capture and paste clear both ground-track reservations and the
  separate runway flight reservation. Orientation bits on hangars and aprons
  remain intact. Rotation/reflection remains a geometric transformation.
- Custom blueprint replay rebuilds every affected station in deterministic
  ID order, including disconnected layouts that fall back to several stations.
- Unsupported raw airport tile types are rejected before terrain changes.
- Equal air-surface sorting priorities use the stable air-type ID as a tie
  breaker; simulation fallback selection no longer relies on equal-key sort
  behavior across standard libraries.

## Review and validation

Reviewed airport tile encoding, runway/path reservations, aircraft state and
NewGRF callback-cache serialization, hangar pool restoration, oil-rig conversion,
airport construction query/execute separation, animation/random triggers,
blueprint command payloads and affected merge interfaces. Local company and
GUI track overlays remain presentation concerns rather than simulation inputs.
Aircraft paths and state-sensitive callback timing retain their saved state.

Windows x64 Release builds. All 257 test cases / 4794 assertions pass. The
hangar identity regression fails before the fix and passes afterward. Coverage
also includes network-restore cache reconstruction, station/depot ID collisions,
removed hangars, reservation sanitizing without orientation loss, invalid
payloads without terrain/random changes and deterministic air-type sorting.
An upstream non-rectangular airport test now cleans its station pool and map.

An isolated diagnostic run with the existing save contained 5 airports,
including 2 custom airports, and one aircraft. Across approximately 4096 game
ticks, 60 client samples matched the server at identical ticks for airport
areas/flags, apron/runway vectors, hangar IDs, depot count, map reservations
and aircraft state/path-cache summaries. No mismatch was observed. Both native
Windows clients became active and the server shut down normally. Temporary
sampling is excluded from the final source. Final executable multiplayer logs
are retained outside the repository.

No save-format extension was removed or newly required. Server and clients
must run matching builds. These checks cover the reviewed implementation and
local scenarios, not every third-party NewGRF combination or long-running
cross-machine game. No PR is created; the user requested a normal push.
