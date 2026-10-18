# Coupling maximum-speed criteria

The coupling criteria window provides two optional numeric comparisons below the relative maximum-speed filters. Each row supports disabled, equal, not equal, less, less/equal, greater, or greater/equal. Both enabled rows must match, allowing a bounded interval such as >= 80 and <= 120. The target's maximum capability is compared, independently of current speed and temporary shunting limits.

A threshold records both its integer value and its velocity unit code. Unit codes 0..4 are mph, km/h, m/s, game units/day, and knots; code 5 is game units/second. Candidate checks convert using that explicit saved unit, never client locale preferences. Value modifications atomically carry the unit code in ModifyOrder's auxiliary byte, otherwise used for cargo. Comparators and values update active shared orders for vehicle and standalone-list edits.

The six numeric values use OrderConditionComparator values + 1, with zero disabled; they occupy flags bits 6..8 and 9..11. Two values and unit codes are included in order-copy operations, insertion/bulk command payloads, regular orders and vehicle current-order save tables. The mandatory couple_speed_filter save feature is version 2; version 1 and older saves load with the new rows disabled. Older executables reject new snapshots rather than silently ignoring the criteria.

The candidate predicate is shared by checkpoint selection, routing, claim validation and physical contact. Tests cover all comparison operators, boundaries, combined criteria, disabled rows, order copies, current shared-order updates, command preflight and serialisation, snapshot fields, explicit unit rounding and client locale independence. Final release suite: 275 cases, 5951 assertions.


## Soft speed preference

Unclaimed checkpoint partners and routing destinations receive a bounded preference for closer maximum speeds. The score combines existing distance/cost with a penalty of ceil(abs(speed difference) / 8), capped at 16 tiles. Equal scores use stable vehicle IDs rather than tile-hash enumeration order. Existing valid exclusive claims are retained. Explicit filters remain hard requirements; preference alone never rejects a partner.

The coupling pathfinder retries its original nearest-reachable search if the preference was used but the search or reservation failed. Other train routing is unchanged. Platform scans deduplicate primary vehicles so long consists do not repeat expensive candidate/approach checks once per wagon. Selection does not consume random numbers.

The two relative filters compare this approaching train with the waiting train. The two numeric rows compare the waiting train against manually entered values; all enabled conditions must pass. The Chinese labels identify both sides explicitly and the help text explains equality, inclusive bounds and an 80..120 example.
