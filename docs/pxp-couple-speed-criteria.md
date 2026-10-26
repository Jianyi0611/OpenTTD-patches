# Coupling maximum-speed criteria

The coupling criteria window shows one speed row with Minimum and Maximum selectors. Each offers Disabled, My top speed and Manual. Numeric fields appear only in Manual mode, and selecting Manual opens an input query. Both bounds are inclusive and apply to the waiting train maximum capability, independently of current speed and temporary shunting limits. Choosing My top speed for both bounds requires equal maximum speeds; disabling both removes the limits.

A threshold records both its integer value and its velocity unit code. Unit codes 0..4 are mph, km/h, m/s, game units/day, and knots; code 5 is game units/second. Candidate checks convert using that explicit saved unit, never client locale preferences. Value modifications atomically carry the unit code in ModifyOrder's auxiliary byte, otherwise used for cargo. Comparators and values update active shared orders for vehicle and standalone-list edits.

Each threshold has a dedicated comparator field using OrderConditionComparator values + 1, with zero disabled. Two values, two unit codes and two comparators are included in order-copy operations, insertion/bulk command payloads, regular orders and vehicle current-order save tables. The mandatory couple_speed_filter save feature is version 3; version 1 and version 2 saves load with both rows disabled. Earlier versions stored the comparators in flags bits 6..8 and 9..11, where bit 7 is also the generic "travel time timetabled" flag; enabling a timetable travel time therefore silently turned the minimum row into "not equal". Older executables reject new snapshots rather than silently ignoring the criteria.

The candidate predicate is shared by checkpoint selection, routing, claim validation and physical contact. Tests cover all comparison operators, boundaries, combined criteria, disabled rows, order copies, current shared-order updates, command preflight and serialisation, snapshot fields, explicit unit rounding and client locale independence. Current suite: 276 cases, 6257 assertions.


## Soft speed preference

Unclaimed checkpoint partners and routing destinations receive a bounded preference for closer maximum speeds. The score combines existing distance/cost with a penalty of ceil(abs(speed difference) / 4), capped at 24 tiles. Equal scores use stable vehicle IDs rather than tile-hash enumeration order. Existing valid exclusive claims are retained. Explicit filters remain hard requirements; preference alone never rejects a partner.

The coupling pathfinder retries its original nearest-reachable search if the preference was used but the search or reservation failed. Other train routing is unchanged. Platform scans deduplicate primary vehicles so long consists do not repeat expensive candidate/approach checks once per wagon. Selection does not consume random numbers.

The compact UI maps Minimum/My top speed to rejecting slower waiting trains and Maximum/My top speed to rejecting faster waiting trains. Manual minimum and maximum use >= and <= respectively. Each source selection is one atomic ModifyOrder command, clearing the corresponding old relative/numeric condition together and updating active shared orders. Previously entered values and units remain available when switching sources. Source selection itself needs no additional save fields beyond the comparator storage described above. Existing saved arbitrary comparisons retain their effect until that selector is changed; a nonstandard comparator is shown alongside its value, and a concurrent legacy relative filter is marked with an asterisk. The order text continues to expose all active conditions.

The stronger score doubles the preference for ordinary speed differences while increasing the maximum detour penalty only from 16 to 24 tiles. A regression scenario prefers a matching train nine tiles away over a nonmatching train one tile away (speed gap 40), while a large detour still selects the nearer partner.


Before a first claim, ready locomotives whose coupling orders enable a speed bound can also compete for the same waiting train. Maximum-speed fit precedes total power, followed by route cost and stable vehicle ID. With both bounds disabled, the extra arbitration is skipped; the ordinary soft speed preference above remains. See [coupling priority](pxp-coupling-power-priority.md) for readiness, budgets and fallback.
