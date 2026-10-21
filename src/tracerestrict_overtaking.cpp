/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 */
/** @file tracerestrict_overtaking.cpp Automatic registration and siding claims reuse rollback-capable slot transactions. */
#include "stdafx.h"
#include "tracerestrict_overtaking.h"
#include "tracerestrict_backup.h"
#include "train.h"
#include "cargotype.h"
#include "rail_map.h"
#include "station_map.h"
#include "pbs.h"
#include "signal_func.h"
#include "pathfinder/follow_track.hpp"
#include "core/backup_type.hpp"
#include "timer/timer_game_tick.h"
#include "vehicle_func.h"
#include "date_func.h"
#include <map>
#include <set>
#include <queue>
#include "safeguards.h"

namespace {
const Train *GetTrain(VehicleID id)
{
	const Vehicle *vehicle = Vehicle::GetIfValid(id);
	return vehicle != nullptr && vehicle->type == VehicleType::Train ? Train::From(vehicle) : nullptr;
}
struct Binding {
	TraceRestrictRefId ref;
	AutoYieldRole role;
	Trackdir td;
	bool platform = false;
	uint16_t speed_cap = 0; ///< Approach only; zero follows the current rail type limit.
	AutoYieldRegistration registration = AutoYieldRegistration::Reservation;
};
struct Area {
	CompanyID owner;
	uint16_t id;
	uint16_t percent = 0; ///< Strictest configured approach threshold; areas without an approach remain disabled.
	std::vector<Binding> bindings;
	TraceRestrictSlot *detection = nullptr;
	TraceRestrictSlot *pending = nullptr; ///< Registered by reservation, not physically past a detector yet.
	std::map<TraceRestrictRefId, TraceRestrictSlot *> lanes;
	std::vector<SignalReference> signal_dependants;
	bool exit_only = false; ///< An exit marks and claims its whole siding when no entrance markers are configured.
	bool enabled = false;
	bool has_platform = false;
	uint main_count = 0; ///< Rebuilt from saved markers; never a local UI preference.
};
using AreaKey = std::pair<CompanyID, uint16_t>;
std::map<AreaKey, Area> areas;
bool dirty = true;
uint probe_depth = 0;
/* Keep each path restriction evaluation bounded. Larger areas degrade to
 * ordinary signal behaviour instead of making route search expensive. */
constexpr size_t MAX_PARTICIPANTS = 16;
constexpr size_t MAX_SIDINGS = 16;
constexpr size_t MAX_APPROACH_MARKERS = 16;
constexpr size_t MAX_MAIN_MARKERS = 16;
constexpr size_t MAX_ROUTE_NODES = 256;
/* A condition or marker may ask several related questions (free siding,
 * evacuation, and route length). Share a budget across those searches. */
constexpr size_t MAX_PROBE_NODES_PER_EVALUATION = 1024;
constexpr uint SAME_SPEED_PASSENGER_GAP = 16; ///< Maximum clear gap behind a freight train, in route tiles.
constexpr uint SHORT_STATION_STOP_TICKS = 256; ///< Never hold traffic indefinitely for station work.
size_t probe_budget_remaining = 0;
struct PassengerPriorityEntry { const Train *train; bool loaded; };
std::array<PassengerPriorityEntry, MAX_PARTICIPANTS> passenger_priority_cache;
size_t passenger_priority_count = 0; // Only valid within one read-only evaluation.
struct MergeProofEntry { const Area *area; const Train *train; TileIndex start; Trackdir td; std::optional<uint> distance; };
std::array<MergeProofEntry, MAX_PARTICIPANTS + 1> merge_proof_cache;
size_t merge_proof_count = 0;
size_t merge_probe_remaining = 0; ///< Shared optional allowance, inside the existing total probe cap.

class AutoYieldProbeBudget {
public:
	AutoYieldProbeBudget() : previous(probe_budget_remaining)
	{
		if (probe_depth == 0) {
			probe_budget_remaining = MAX_PROBE_NODES_PER_EVALUATION;
			passenger_priority_count = 0;
			merge_proof_count = 0;
			merge_probe_remaining = MAX_ROUTE_NODES;
		}
	}
	~AutoYieldProbeBudget() { probe_budget_remaining = previous; }
private:
	size_t previous;
};

/** A soft preference must leave room for the authoritative exit/claim checks. */
class AutoYieldPreferenceBudget {
public:
	explicit AutoYieldPreferenceBudget(size_t limit = MAX_ROUTE_NODES) : previous(probe_budget_remaining), allowance(std::min(probe_budget_remaining / 2, limit))
	{
		probe_budget_remaining = allowance;
	}
	~AutoYieldPreferenceBudget() { probe_budget_remaining = previous - (allowance - probe_budget_remaining); }
private:
	size_t previous;
	size_t allowance;
};

bool Active(const Train *v)
{
	return v != nullptr && v->IsPrimaryVehicle() && !v->IsVirtualOrCarried() && !v->IsInDepot() && IsValidTile(v->tile) &&
			!v->vehstatus.Any({VehState::Stopped, VehState::Crashed}) && v->GetDisplayMaxSpeed() > 0;
}
const Area *FindArea(const Train *v, uint16_t id)
{
	if (v == nullptr || probe_depth != 0 || dirty) return nullptr;
	const TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(id);
	if (slot == nullptr || slot->automatic_yield_area != 0 || slot->vehicle_type != VehicleType::Train) return nullptr;
	auto found = areas.find({CompanyID{slot->owner}, id});
	if (found == areas.end() || !found->second.enabled || found->second.detection == nullptr || found->second.detection->occupants.size() > MAX_PARTICIPANTS) return nullptr;
	return &found->second;
}
/** Same effective cap for every member prevents contradictory priorities after
 * entering through different detectors. Rail limits are read live (rail conversion
 * must not leave a stale cached cap); rail type zero means unrestricted. */
uint DetectionSpeed(const Area &area, const Train *v)
{
	uint speed = v->GetDisplayMaxSpeed();
	for (const Binding &binding : area.bindings) {
		if (binding.role != AutoYieldRole::Approach) continue;
		const TileIndex tile = GetTraceRestrictRefIdTileIndex(binding.ref);
		const uint cap = binding.speed_cap != 0 ? binding.speed_cap : GetRailTypeInfo(GetRailTypeByTrack(tile, GetTraceRestrictRefIdTrack(binding.ref)))->max_speed;
		if (cap != 0) speed = std::min(speed, cap);
	}
	return speed;
}
/** Actual passengers, including NewGRF passenger classes and articulated parts.
 * Cache only for this evaluation: loading, unloading and refits are visible next
 * time without persisted fields or client-dependent invalidation. */
bool HasPassengerPriority(const Train *v)
{
	for (size_t i = 0; i < passenger_priority_count; i++) if (passenger_priority_cache[i].train == v) return passenger_priority_cache[i].loaded;
	bool loaded = false;
	for (const Train *part = v; part != nullptr; part = part->Next()) {
		if (part->cargo_type < NUM_CARGO && IsCargoInClass(part->cargo_type, CargoClass::Passengers) &&
				(part->cargo.StoredCount() != 0 || (v->current_order.IsAnyLoadingType() && part->cargo_cap != 0))) { loaded = true; break; }
	}
	if (passenger_priority_count < passenger_priority_cache.size()) passenger_priority_cache[passenger_priority_count++] = {v, loaded};
	return loaded;
}
bool HigherPriority(const Area &area, const Train *a, const Train *b, uint percent)
{
	const uint64_t as = DetectionSpeed(area, a), bs = DetectionSpeed(area, b);
	return as == bs ? HasPassengerPriority(a) && !HasPassengerPriority(b) : as > bs && as * 100 >= bs * (100 + percent);
}
const Binding *FindBinding(const Area &area, TraceRestrictRefId ref)
{
	for (const Binding &binding : area.bindings) if (binding.ref == ref) return &binding;
	return nullptr;
}
bool AtRole(const Area &area, TileIndex tile, Trackdir td, AutoYieldRole role)
{
	for (const Binding &binding : area.bindings) if (binding.role == role && binding.ref == MakeTraceRestrictRefId(tile, TrackdirToTrack(td)) && binding.td == td) return true;
	return false;
}

/** Check every physical tile, including station tiles skipped by the follower. */
bool FreeTrackTile(const Train *v, TileIndex tile, Trackdir td, const Train *departing = nullptr)
{
	const Track track = TrackdirToTrack(td);
	bool blocked = false;
	for (const Train *part : VehiclesOnTile<VehicleType::Train>(tile)) {
		if (part->Primary() != v->Primary() && (departing == nullptr || part->Primary() != departing->Primary()) && ((part->track & TrackToTrackBits(track)) != TRACK_BIT_NONE || TracksOverlap(part->track | TrackToTrackBits(track)))) {
			blocked = true;
			break;
		}
	}
	if (blocked) return false;
	TrackBits reserved = GetReservedTrackbits(tile);
	if ((reserved & TrackToTrackBits(track)) == TRACK_BIT_NONE && TracksOverlap(reserved | TrackToTrackBits(track))) {
		/* A tentative main-line reservation can cross this alternative at a
		 * junction. As with reservations on this exact track, distinguish our
		 * own route (or the hypothetical departure) from another train's route.
		 * This filters only the read-only proof; PBS still owns every real bit. */
		for (TrackBits bits = reserved; bits != TRACK_BIT_NONE; bits = KillFirstBit(bits)) {
			const Track reserved_track = FindFirstTrack(bits);
			const Train *owner = GetTrainForReservation(tile, reserved_track);
			if (owner != nullptr && (owner->Primary() == v->Primary() || (departing != nullptr && owner->Primary() == departing->Primary()))) reserved &= ~TrackToTrackBits(reserved_track);
		}
		if (TracksOverlap(reserved | TrackToTrackBits(track))) return false;
	}
	if ((reserved & TrackToTrackBits(track)) != TRACK_BIT_NONE) {
		const Train *owner = GetTrainForReservation(tile, track);
		if (owner == nullptr || (owner->Primary() != v->Primary() && (departing == nullptr || owner->Primary() != departing->Primary()))) return false;
	}
	const bool signal_tile = IsPlainRailTile(tile) ||
			(IsTileType(tile, TileType::TunnelBridge) && IsTunnelBridgeWithSignalSimulation(tile));
	if (const TraceRestrictProgram *program = signal_tile ? GetExistingTraceRestrictProgram(tile, track) : nullptr; program != nullptr) {
		TraceRestrictProgramInput input(tile, td, nullptr, nullptr);
		TraceRestrictProgramResult out;
		program->Execute(v, input, out);
		if (out.flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) return false;
	}
	return true;
}

/** Bounded, read-only path proof. Ordinary restrictions and PBS ownership still apply.
 * Unknown/budget exhausted means no proof, never permission to force a train.
 * departing is used only for a hypothetical evacuation, never for admission. */
std::optional<uint> RouteToRole(const Area &area, const Train *v, TileIndex start, Trackdir start_td, AutoYieldRole target, bool free_path, std::optional<TraceRestrictRefId> exact_ref = std::nullopt, bool *search_incomplete = nullptr, const Train *departing = nullptr, std::optional<TraceRestrictRefId> *matched_ref = nullptr, std::optional<TraceRestrictRefId> excluded_ref = std::nullopt, bool require_clearance = false)
{
	if (search_incomplete != nullptr) *search_incomplete = false;
	if (matched_ref != nullptr) matched_ref->reset();
	if (!IsValidTile(start) || !IsValidTrackdir(start_td) || !v->railtypes.Any()) return std::nullopt;
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	struct Node { TileIndex tile; Trackdir td; uint distance; };
	std::queue<Node> queue;
	std::set<std::pair<TileIndex, Trackdir>> seen;
	queue.push({start, start_td, 0});
	while (!queue.empty() && seen.size() < MAX_ROUTE_NODES && probe_budget_remaining > 0) {
		const Node current = queue.front(); queue.pop();
		if (!seen.insert({current.tile, current.td}).second) continue;
		probe_budget_remaining--;
		/* The geometric follower does not enforce signal direction. Match the
		 * existing train rules without changing the pathfinder itself. */
		if (HasOnewaySignalBlockingTrackdir(current.tile, current.td)) continue;
		if (free_path) {
			if (!FreeTrackTile(v, current.tile, current.td, departing)) continue;
		}
		if (AtRole(area, current.tile, current.td, target)) {
			const auto ref = MakeTraceRestrictRefId(current.tile, TrackdirToTrack(current.td));
			if ((!exact_ref || *exact_ref == ref) && (!excluded_ref || *excluded_ref != ref)) {
				/* Try the other branches if this parallel Main has no proven
				 * free departure; a third usable main must not be overlooked. */
				if (require_clearance && !RouteToRole(area, v, current.tile, current.td, AutoYieldRole::Clear, true,
						std::nullopt, nullptr, departing)) continue;
				if (matched_ref != nullptr) *matched_ref = ref;
				return current.distance;
			}
			/* A second marker downstream on the same track is not a parallel
			 * escape route. Only the first Main on each branch participates. */
			if (target == AutoYieldRole::Main) continue;
		}
		{
			CFollowTrackRail follower(v);
			if (!follower.Follow(current.tile, current.td)) continue;
			/* A platform needs per-tile checks. An ordinary tunnel/bridge can
			 * be proved through its two PBS ends and the existing vehicle query;
			 * do not inspect surface tiles underneath it as railway tiles. Signal
			 * simulation is accepted only when the entire span is empty and both
			 * ends unreserved; partial block availability is never guessed. */
			if (free_path && (follower.tiles_skipped != 0 || follower.is_tunnel || follower.is_bridge)) {
				if (size_t(follower.tiles_skipped) > probe_budget_remaining) continue;
				if (follower.is_station) {
					bool clear = true;
					const TileIndexDiff step = TileOffsByDiagDir(follower.exitdir);
					for (int i = follower.tiles_skipped; i > 0; --i) {
						probe_budget_remaining--;
						if (!FreeTrackTile(v, follower.new_tile - step * i, FindFirstTrackdir(follower.new_td_bits), departing)) { clear = false; break; }
					}
					if (!clear) continue;
				} else {
					if (!follower.is_tunnel && !follower.is_bridge) continue;
					const bool simulated = IsTunnelBridgeWithSignalSimulation(current.tile) || IsTunnelBridgeWithSignalSimulation(follower.new_tile);
					if (simulated && (HasAcrossTunnelBridgeReservation(current.tile) || HasAcrossTunnelBridgeReservation(follower.new_tile))) continue;
					if (TunnelBridgeIsFree(current.tile, follower.new_tile, simulated ? nullptr : v, TBIFM_ACROSS_ONLY).Failed()) continue;
					probe_budget_remaining -= follower.tiles_skipped;
				}
			}
			for (TrackdirBits bits = follower.new_td_bits; bits != TRACKDIR_BIT_NONE; bits = KillFirstBit(bits)) {
				queue.push({follower.new_tile, FindFirstTrackdir(bits), current.distance + uint(1 + follower.tiles_skipped)});
			}
		}
	}
	if (search_incomplete != nullptr) *search_incomplete = !queue.empty();
	return std::nullopt;
}

/** Infer an exclusive siding from its exit, without searching alternate routes.
 * Backward traversal must be unique and end at a forward divergence. A backward
 * merge, crossing, tunnel/bridge wormhole or exhausted budget is not proof. */
std::optional<uint> InferredSidingLength(const Area &area, const Train *v, const Binding &exit, std::vector<StationID> *stations = nullptr)
{
	TileIndex tile = GetTraceRestrictRefIdTileIndex(exit.ref);
	Trackdir reverse_td = ReverseTrackdir(exit.td);
	uint distance = 0;
	std::set<std::pair<TileIndex, Trackdir>> seen;
	while (seen.size() < MAX_ROUTE_NODES && probe_budget_remaining > 0) {
		if (!seen.emplace(tile, reverse_td).second) return std::nullopt;
		/* This is a backwards geometry scan; legality must be checked in the
		 * train's forward direction, not in the direction of this scan. */
		if (HasOnewaySignalBlockingTrackdir(tile, ReverseTrackdir(reverse_td))) return std::nullopt;
		if (AtRole(area, tile, ReverseTrackdir(reverse_td), AutoYieldRole::Main)) return std::nullopt;
		probe_budget_remaining--;
		CFollowTrackRail backward(v);
		if (!backward.Follow(tile, reverse_td) || (backward.tiles_skipped != 0 && !backward.is_station) ||
				(!IsPlainRailTile(backward.new_tile) && !HasStationTileRail(backward.new_tile)) || backward.new_td_bits == TRACKDIR_BIT_NONE || KillFirstBit(backward.new_td_bits) != TRACKDIR_BIT_NONE) return std::nullopt;
		const Trackdir next_td = FindFirstTrackdir(backward.new_td_bits);
		if (stations != nullptr && IsRailStationTile(backward.new_tile) && !IsRailWaypoint(backward.new_tile)) {
			const StationID station = GetStationIndex(backward.new_tile);
			if (std::find(stations->begin(), stations->end(), station) == stations->end()) stations->push_back(station);
		}
		if (size_t(backward.tiles_skipped) > probe_budget_remaining) return std::nullopt;
		probe_budget_remaining -= backward.tiles_skipped;
		if (IsPlainRailTile(backward.new_tile) && KillFirstBit(GetTrackBits(backward.new_tile)) != TRACK_BIT_NONE) {
			if (probe_budget_remaining < 2) return std::nullopt;
			probe_budget_remaining -= 2;
			CFollowTrackRail upstream(v);
			if (!upstream.Follow(backward.new_tile, next_td) || upstream.tiles_skipped != 0 ||
					upstream.new_td_bits == TRACKDIR_BIT_NONE || KillFirstBit(upstream.new_td_bits) != TRACKDIR_BIT_NONE) return std::nullopt;
			CFollowTrackRail forward(v);
			if (!forward.Follow(upstream.new_tile, ReverseTrackdir(FindFirstTrackdir(upstream.new_td_bits))) ||
					forward.new_tile != backward.new_tile || forward.tiles_skipped != 0) return std::nullopt;
			if (KillFirstBit(forward.new_td_bits) != TRACKDIR_BIT_NONE) return distance;
			/* Two disjoint parallel curves can share a tile. They are not a
			 * junction: keep tracing this unique track instead of rejecting the
			 * siding. Actual overlapping crossings still provide no proof. */
			if (forward.new_td_bits == TRACKDIR_BIT_NONE || TracksOverlap(GetTrackBits(backward.new_tile))) return std::nullopt;
		}
		tile = backward.new_tile;
		reverse_td = next_td;
		distance += 1 + backward.tiles_skipped;
	}
	return std::nullopt;
}

bool HasLaneClaim(const Area &area, const Train *v)
{
	for (const auto &[ref, lane] : area.lanes) if (lane->IsOccupant(v->index)) return true;
	return false;
}

struct PlatformInfo {
	bool marked = false;
	bool known = false;
	std::optional<TraceRestrictRefId> exit;
	std::vector<StationID> stations;
};

/** Resolve either the exit-only layout or an optional legacy entrance marker. */
PlatformInfo GetPlatformInfo(const Area &area, const Train *v, TraceRestrictRefId ref)
{
	PlatformInfo result;
	if (!area.has_platform) return result;
	const Binding *binding = FindBinding(area, ref);
	if (binding == nullptr) return result;
	const Binding *exit = nullptr;
	if (binding->role == AutoYieldRole::Exit) {
		result.marked = binding->platform;
		if (result.marked) exit = binding;
	} else if (binding->role == AutoYieldRole::Siding) {
		result.marked = binding->platform;
		for (const Binding &candidate : area.bindings) {
			if (candidate.role != AutoYieldRole::Exit || (!binding->platform && !candidate.platform)) continue;
			if (!RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Exit, false, candidate.ref)) continue;
			result.marked = true;
			exit = &candidate;
			break;
		}
	}
	if (exit != nullptr) {
		result.exit = exit->ref;
		result.known = InferredSidingLength(area, v, *exit, &result.stations).has_value();
	}
	return result;
}

/** Only a positively identified, scheduled stop can request a platform turn. */
bool ScheduledPlatformStop(const Train *v, const PlatformInfo &platform)
{
	if (!platform.marked || !platform.known || !v->current_order.IsType(OT_GOTO_STATION)) return false;
	return std::any_of(platform.stations.begin(), platform.stations.end(), [v](StationID station) {
		return v->current_order.GetDestination().ToStationID() == station && v->current_order.ShouldStopAtStation(v, station, false);
	});
}

/** Preserve mandatory station, waypoint and PXP coupling destinations. */
bool RequiresPlatform(const Train *v, const PlatformInfo &platform)
{
	if (!platform.known) return true; // An unknown layout cannot justify rejecting a route.
	if (v->current_order.IsAnyLoadingType() || v->current_order.IsType(OT_GOTO_WAYPOINT) ||
			v->current_order.IsType(OT_GOTO_COUPLE) || v->current_order.IsType(OT_WAIT_COUPLE) ||
			v->current_order.IsType(OT_DECOUPLE)) return true;
	for (StationID station : platform.stations) if (v->current_order.ShouldStopAtStation(v, station, false)) return true;
	return false;
}

bool DoingStationWork(const Train *v)
{
	return v->current_order.IsAnyLoadingType() || v->current_order.IsType(OT_WAIT_COUPLE) ||
			v->current_order.IsType(OT_DECOUPLE) || (v->current_order.IsType(OT_WAITING) && v->cur_speed == 0);
}

/** Station work must not become an in-transit priority or a forced evacuation. */
bool ServingPlatform(const Area &area, const Train *v)
{
	if (!area.has_platform || v == nullptr || !DoingStationWork(v)) return false;
	StationID station = v->last_station_visited;
	if (v->current_order.IsType(OT_LOADING_ADVANCE)) station = v->current_order.GetDestination().ToStationID();
	if (v->current_order.IsType(OT_WAIT_COUPLE) && v->current_order.HasWaitCoupleStation()) station = v->current_order.GetWaitCoupleStation();
	if (station == StationID::Invalid()) return false;
	for (const auto &[ref, lane] : area.lanes) {
		const PlatformInfo platform = GetPlatformInfo(area, v, ref);
		if (platform.marked && platform.known && std::find(platform.stations.begin(), platform.stations.end(), station) != platform.stations.end()) {
			if (lane->IsOccupant(v->index)) return true;
			/* A station can span marked and unmarked tracks. Without a lane
			 * claim, prove that an actual station part can reach this exit. This
			 * also covers long coupled trains with the front outside the platform. */
			for (const Train *part = v; part != nullptr; part = part->Next()) {
				if (probe_budget_remaining == 0) break;
				probe_budget_remaining--;
				if (!IsValidTile(part->tile) || !IsRailStationTile(part->tile) || GetStationIndex(part->tile) != station) continue;
				if (RouteToRole(area, v, part->tile, part->GetVehicleTrackdir(), AutoYieldRole::Exit, false, platform.exit)) return true;
				if (probe_budget_remaining == 0) break;
			}
		}
		if (probe_budget_remaining == 0) break;
	}
	return false;
}

struct MainApproach {
	const Train *train;
	uint distance;
	std::optional<TraceRestrictRefId> main;
	bool clearance_only = false; ///< Requires a positive merge proof; not an unknown follower.
	uint priority_gap = UINT_MAX; ///< Precomputed before sorting; probes never run in a comparator.
};
/** Same-speed priority is local. Compare forward routes to the common clearance
 * marker, including the freight consist length, rather than map distance (which
 * could confuse opposite directions or unrelated tracks). An incomplete proof
 * cannot introduce a new same-speed wait. Actual PBS remains authoritative. */
bool ClosePassenger(const Area &area, const Train *passenger, const Train *freight)
{
	AutoYieldPreferenceBudget preference_budget;
	const Train *p = passenger->GetMovingFront(), *f = freight->GetMovingFront();
	const auto pd = RouteToRole(area, passenger, p->tile, p->GetVehicleTrackdir(), AutoYieldRole::Clear, false);
	const auto fd = RouteToRole(area, freight, f->tile, f->GetVehicleTrackdir(), AutoYieldRole::Clear, false);
	if (!pd || !fd) return false;
	if (*pd >= *fd) return *pd - *fd <= SAME_SPEED_PASSENGER_GAP + CeilDiv(freight->gcache.cached_total_length, TILE_SIZE);
	/* Once it is alongside/ahead, protect its tail through the shared merge. */
	return *fd - *pd <= SAME_SPEED_PASSENGER_GAP + CeilDiv(passenger->gcache.cached_total_length, TILE_SIZE);
}

/** Loading is not loss of priority. Preserve a normal short stop, but release
 * indefinite full-load/cargo-specific waits and coupling work. All inputs are
 * saved simulation state; no wall clock, local player or persistent cache. */
bool StationPriorityPaused(const Area &area, const Train *v)
{
	if (!ServingPlatform(area, v)) return false;
	return !v->current_order.IsAnyLoadingType() || v->current_order.IsFullLoadOrder() ||
			v->current_order.GetLoadType() == OrderLoadType::CargoTypeLoad ||
			v->current_order.GetWaitTime() > SHORT_STATION_STOP_TICKS || v->current_order_time >= SHORT_STATION_STOP_TICKS;
}
std::vector<MainApproach> GetMainApproaches(const Area &area, const Train *focus = nullptr)
{
	if (area.main_count == 0) return {};
	std::vector<MainApproach> result;
	result.reserve(area.detection->occupants.size() + 1);
	/* Classifying distant members must leave room to prove admission to a
	 * free siding. Resolve the train choosing its branch first; otherwise
	 * earlier slot occupants can consume even its own route proof. Unknown
	 * followers retain the existing conservative priority handling. */
	std::optional<AutoYieldPreferenceBudget> classification_budget;
	if (focus != nullptr) classification_budget.emplace(MAX_PROBE_NODES_PER_EVALUATION / 2);
	const auto append = [&](const Train *train) {
		if (!Active(train) || ServingPlatform(area, train)) return;
		bool incomplete = false;
		std::optional<TraceRestrictRefId> main;
		auto distance = RouteToRole(area, train, train->GetMovingFront()->tile, train->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Main, false, std::nullopt, &incomplete, nullptr, &main);
		/* Registered members beyond the local route-proof budget remain
		 * potential followers. A computational cap must not become a hidden
		 * detection distance. Unknown membership never grants track access. */
		if (distance || incomplete) result.push_back({train, distance.value_or(UINT_MAX), main});
		else if (area.main_count > 1 && !HasLaneClaim(area, train)) result.push_back({train, UINT_MAX, std::nullopt, true});
	};
	if (focus != nullptr) append(focus);
	for (VehicleID id : area.detection->occupants) {
		const Train *train = GetTrain(id);
		if (train != focus) append(train);
	}
	return result;
}
bool FreeLane(const Area &area, const Train *v, bool approaching = false, bool ordinary_only = false, std::optional<TraceRestrictRefId> exact_ref = std::nullopt);

/** Prove a unique forward continuation to the area's directed clearance point.
 * A branch, loop, opposing signal or exhausted budget cannot establish a shared
 * bottleneck. Results live only in this evaluation; rail edits and joining
 * clients read the same saved layout, without a persistent topology cache. */
std::optional<uint> MergeClearanceDistance(const Area &area, const MainApproach &approach)
{
	if (!approach.main && !approach.clearance_only) return std::nullopt;
	const Binding *main = approach.main ? FindBinding(area, *approach.main) : nullptr;
	if (approach.main && main == nullptr) return std::nullopt;
	const Train *front = approach.train->GetMovingFront();
	const TileIndex start = main != nullptr ? GetTraceRestrictRefIdTileIndex(main->ref) : front->tile;
	const Trackdir start_td = main != nullptr ? main->td : front->GetVehicleTrackdir();
	const auto total = [&](std::optional<uint> distance) -> std::optional<uint> {
		if (!distance || (main != nullptr && approach.distance == UINT_MAX)) return std::nullopt;
		return *distance + (main != nullptr ? approach.distance : 0);
	};
	for (size_t i = 0; i < merge_proof_count; i++) {
		const auto &entry = merge_proof_cache[i];
		if (entry.area == &area && entry.train == approach.train && entry.start == start && entry.td == start_td) return total(entry.distance);
	}
	std::optional<uint> distance;
	{
		AutoYieldPreferenceBudget preference_budget;
		AutoRestoreBackup guard(probe_depth, probe_depth + 1);
		TileIndex tile = start;
		Trackdir td = start_td;
		uint travelled = 0;
		std::set<std::pair<TileIndex, Trackdir>> seen;
		while (probe_budget_remaining > 0 && merge_probe_remaining > 0 && seen.size() < MAX_ROUTE_NODES) {
			if (!IsValidTile(tile) || !IsValidTrackdir(td) || !seen.emplace(tile, td).second) break;
			probe_budget_remaining--;
			merge_probe_remaining--;
			if (HasOnewaySignalBlockingTrackdir(tile, td)) break;
			const bool signal_tile = IsPlainRailTile(tile) ||
					(IsTileType(tile, TileType::TunnelBridge) && IsTunnelBridgeWithSignalSimulation(tile));
			if (const TraceRestrictProgram *program = signal_tile ? GetExistingTraceRestrictProgram(tile, TrackdirToTrack(td)) : nullptr; program != nullptr) {
				TraceRestrictProgramInput input(tile, td, nullptr, nullptr);
				TraceRestrictProgramResult out;
				program->Execute(approach.train, input, out);
				if (out.flags & (TRPRF_DENY | TRPRF_WAIT_AT_PBS)) break;
			}
			if (AtRole(area, tile, td, AutoYieldRole::Clear)) { distance = travelled; break; }
			/* Serial controls on one line are not two independent feeders. */
			if (tile != start && AtRole(area, tile, td, AutoYieldRole::Main)) break;
			CFollowTrackRail follow(approach.train);
			if (!follow.Follow(tile, td) || follow.new_td_bits == TRACKDIR_BIT_NONE || KillFirstBit(follow.new_td_bits) != TRACKDIR_BIT_NONE) break;
			tile = follow.new_tile;
			td = FindFirstTrackdir(follow.new_td_bits);
			travelled += 1 + follow.tiles_skipped;
		}
	}
	if (merge_proof_count < merge_proof_cache.size()) merge_proof_cache[merge_proof_count++] = {&area, approach.train, start, start_td, distance};
	return total(distance);
}

/** Different Main distances are comparable only after a positive common-exit proof. */
bool FollowingThreat(const Area &area, const MainApproach &fast, const MainApproach &slow, bool *merging = nullptr)
{
	if (fast.train == slow.train || !HigherPriority(area, fast.train, slow.train, area.percent)) return false;
	const bool distinct = fast.clearance_only || slow.clearance_only || (fast.main && slow.main && fast.main != slow.main);
	if (distinct) {
		const auto fd = MergeClearanceDistance(area, fast), sd = MergeClearanceDistance(area, slow);
		if (!fd || !sd || *fd < *sd) return false;
		if (DetectionSpeed(area, fast.train) == DetectionSpeed(area, slow.train) &&
				*fd - *sd > SAME_SPEED_PASSENGER_GAP + CeilDiv(slow.train->gcache.cached_total_length, TILE_SIZE)) return false;
		if (merging != nullptr) *merging = true;
		return true;
	}
	return fast.distance > slow.distance &&
			(DetectionSpeed(area, fast.train) != DetectionSpeed(area, slow.train) ||
					(fast.main && slow.main && fast.distance != UINT_MAX && slow.distance != UINT_MAX &&
					 fast.distance - slow.distance <= SAME_SPEED_PASSENGER_GAP + CeilDiv(slow.train->gcache.cached_total_length, TILE_SIZE)));
}

/** Slowest first; among otherwise equal contenders prefer the closest follower gap. */
void SetApproachGaps(const Area &area, std::vector<MainApproach> &candidates, const std::vector<MainApproach> &approaches)
{
	for (MainApproach &slow : candidates) {
		slow.priority_gap=UINT_MAX;
		for (const MainApproach &fast : approaches) {
			if (!FollowingThreat(area,fast,slow)) continue;
			if (!fast.clearance_only && !slow.clearance_only && fast.main && fast.main==slow.main && fast.distance!=UINT_MAX && slow.distance!=UINT_MAX) {
				slow.priority_gap=std::min(slow.priority_gap,fast.distance-slow.distance);
			} else {
				const auto fd=MergeClearanceDistance(area,fast),sd=MergeClearanceDistance(area,slow);
				if (fd && sd && *fd>=*sd) slow.priority_gap=std::min(slow.priority_gap,*fd-*sd);
			}
		}
	}
}

/** A proven free parallel Main avoids an unnecessary diversion. This is only
 * a read-only preference: normal pathfinding and PBS still choose/reserve it. */
bool FreeParallelMain(const Area &area, const MainApproach &approach)
{
	if (area.main_count <= 1 || !approach.main) return false;
	AutoYieldPreferenceBudget preference_budget;
	const Train *front = approach.train->GetMovingFront();
	return RouteToRole(area, approach.train, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Main, true,
			std::nullopt, nullptr, nullptr, nullptr, approach.main, true).has_value();
}

bool NeedsSiding(const Area &area, const Train *v, const std::vector<MainApproach> *known_approaches = nullptr)
{
	if (!Active(v)) return false;
	MainApproach own{v, UINT_MAX, std::nullopt};
	std::vector<MainApproach> approaches = known_approaches != nullptr ? *known_approaches : GetMainApproaches(area, v);
	for (const MainApproach &approach : approaches) if (approach.train == v) own = approach;
	if (own.distance == UINT_MAX) {
		auto distance = RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Main, false, std::nullopt, nullptr, nullptr, &own.main);
		if (!distance) return false;
		own.distance = *distance;
	}
	const auto threatened = [&](const MainApproach &slow, bool *merging = nullptr) {
		bool found = false;
		for (const MainApproach &approach : approaches) {
			bool joined = false;
			if (!FollowingThreat(area, approach, slow, &joined)) continue;
			found = true;
			if (merging != nullptr && joined) *merging = true;
		}
		return found;
	};
	bool merging = false;
	if (!threatened(own, &merging) || (!merging && FreeParallelMain(area, own))) return false;
	/* Allocate scarce sidings to the slowest reachable contenders first.
	 * Existing claims remain authoritative; no train is held on the main line
	 * merely to wait for a slower train to arrive. Equal speeds use distance,
	 * then the stable vehicle ID, never iteration order or random numbers. */
	if (HasLaneClaim(area, v)) return true;
	size_t available = 0;
	for (const auto &[ref, lane] : area.lanes) if (lane->occupants.empty()) available++;
	if (available == 0) return true; // Preserve the existing safe evacuation path.
	SetApproachGaps(area,approaches,approaches);
	std::sort(approaches.begin(), approaches.end(), [&area](const MainApproach &a, const MainApproach &b) {
		return std::tuple{DetectionSpeed(area, a.train), HasPassengerPriority(a.train), a.priority_gap, a.distance, a.train->index} <
				std::tuple{DetectionSpeed(area, b.train), HasPassengerPriority(b.train), b.priority_gap, b.distance, b.train->index};
	});
	size_t priority = 0;
	for (const MainApproach &approach : approaches) {
		if (approach.train == v) break;
		if (DetectionSpeed(area, approach.train) > DetectionSpeed(area, v)) break;
		bool contender_merging = false;
		if (HasLaneClaim(area, approach.train) || !threatened(approach, &contender_merging)) continue;
		if ((contender_merging || !FreeParallelMain(area, approach)) && FreeLane(area, approach.train, true) && ++priority >= available) return false;
		if (probe_budget_remaining == 0) return false;
	}
	return true;
}
/** Unknown/distant routes retain priority; only a positive reverse proof excludes a train. */
bool ProvenOpposing(const Area &area, const Train *v)
{
	const Train *front = v->GetMovingFront();
	bool incomplete = false;
	if (RouteToRole(area, v, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Main, false, std::nullopt, &incomplete) || incomplete) return false;
	/* Passing trains may no longer reach Main. Preserve their priority on
	 * the route to Clear, and while their body is still crossing that marker. */
	if (RouteToRole(area, v, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Clear, false, std::nullopt, &incomplete) || incomplete) return false;
	for (const Train *part = v; part != nullptr; part = part->Next()) {
		if (probe_budget_remaining == 0) return false;
		probe_budget_remaining--;
		if (AtRole(area, part->tile, part->GetVehicleTrackdir(), AutoYieldRole::Clear)) return false;
	}
	return RouteToRole(area, v, front->tile, ReverseTrackdir(front->GetVehicleTrackdir()), AutoYieldRole::Main, false).has_value();
}

bool MustWait(const Area &area, const Train *v)
{
	if (!area.detection->IsOccupant(v->index)) return false;
	std::array<const Train *, MAX_PARTICIPANTS> faster{};
	size_t count = 0;
	for (VehicleID id : area.detection->occupants) {
		const Train *other = GetTrain(id);
		if (other != v && Active(other) && HigherPriority(area, other, v, area.percent) && !StationPriorityPaused(area, other) &&
				(DetectionSpeed(area, other) != DetectionSpeed(area, v) || ClosePassenger(area, other, v))) {
			if (count == faster.size()) return true;
			faster[count++] = other;
		}
	}
	/* Optional direction classification shares a small budget across all
	 * candidates, leaving the original evacuation and claim checks room. */
	AutoYieldPreferenceBudget direction_budget;
	for (size_t i = 0; i < count; i++) if (!ProvenOpposing(area, faster[i])) return true;
	return false;
}

/** A nearby same-speed passenger already in a siding is not a following
 * approach. Protect its departure at Main as well, but only with a positive
 * free route proof so an unreachable/blocked siding cannot freeze main traffic. */
bool PrioritisePassengerExit(const Area &area, const Train *incoming)
{
	if (HasPassengerPriority(incoming) || HasLaneClaim(area, incoming)) return false;
	AutoYieldPreferenceBudget preference_budget;
	for (const auto &[ref, lane] : area.lanes) for (VehicleID id : lane->occupants) {
		const Train *passenger = GetTrain(id);
		if (passenger == incoming || !Active(passenger) || !area.detection->IsOccupant(id) ||
				DetectionSpeed(area, passenger) != DetectionSpeed(area, incoming) || !HasPassengerPriority(passenger) ||
				StationPriorityPaused(area, passenger) || !ClosePassenger(area, passenger, incoming)) continue;
		const Train *front = passenger->GetMovingFront();
		if (RouteToRole(area, passenger, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Clear, true)) return true;
	}
	return false;
}
bool FreeLane(const Area &area, const Train *v, bool approaching, bool ordinary_only, std::optional<TraceRestrictRefId> exact_ref)
{
	/* Priority considers a contender's reachable branch even while a train
	 * ahead temporarily occupies the shared approach. Actual diversion and
	 * admission always require the original full free-path proof. */
	for (const auto &[ref, lane] : area.lanes) {
		if (exact_ref && ref != *exact_ref) continue;
		if (!lane->occupants.empty() && !lane->IsOccupant(v->index)) continue;
		const Binding *binding = FindBinding(area, ref);
		if (binding == nullptr) continue;
		if (ordinary_only && GetPlatformInfo(area, v, ref).marked) continue;
		if (area.exit_only) {
			const auto length = InferredSidingLength(area, v, *binding);
			if (length && *length * TILE_SIZE >= v->gcache.cached_total_length + TILE_SIZE &&
					RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Exit, !approaching, ref) &&
					RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Clear, true)) return true;
			continue;
		}
		if (!RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Siding, !approaching, ref)) continue;
		const auto length = RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Exit, true);
		if (length && *length * TILE_SIZE >= v->gcache.cached_total_length + TILE_SIZE &&
				RouteToRole(area, v, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Clear, true)) return true;
	}
	return false;
}
constexpr uint EQUAL_SPEED_TURNOVER_GAP = 32;

/** Confirm an actual stopped queue, not just distances on adjacent feeders. */
bool StoppedQueueBehind(const Train *arrival, const Train *follower)
{
	if (arrival->cur_speed != 0 || follower->cur_speed != 0) return false;
	AutoYieldPreferenceBudget budget;
	AutoRestoreBackup guard(probe_depth, probe_depth + 1);
	const Train *front = follower->GetMovingFront();
	TileIndex tile = front->tile;
	Trackdir td = front->GetVehicleTrackdir();
	uint travelled = 0;
	const auto contact = [&](TileIndex at, Trackdir direction) {
		for (const Train *part : VehiclesOnTile<VehicleType::Train>(at)) {
			if (part->Primary() == arrival->Primary() &&
					(part->track & TrackToTrackBits(TrackdirToTrack(direction)))) return true;
		}
		return false;
	};
	while (travelled <= EQUAL_SPEED_TURNOVER_GAP && probe_budget_remaining > 0) {
		probe_budget_remaining--;
		if (contact(tile,td)) return true;
		if (HasOnewaySignalBlockingTrackdir(tile,td)) return false;
		CFollowTrackRail follow(follower);
		if (!follow.Follow(tile,td) || follow.new_td_bits == TRACKDIR_BIT_NONE || KillFirstBit(follow.new_td_bits) != TRACKDIR_BIT_NONE) return false;
		const Trackdir next_td = FindFirstTrackdir(follow.new_td_bits);
		if (follow.is_station) {
			const TileIndexDiff step = TileOffsByDiagDir(follow.exitdir);
			for (int i=follow.tiles_skipped; i>0; --i) {
				if (++travelled > EQUAL_SPEED_TURNOVER_GAP || probe_budget_remaining == 0) return false;
				probe_budget_remaining--;
				if (contact(follow.new_tile-step*i,next_td)) return true;
			}
		} else if (follow.tiles_skipped > 0) return false;
		travelled++;
		tile=follow.new_tile;td=next_td;
	}
	return false;
}

bool PassengerService(const Train *v)
{
	for (const Train *part=v->First(); part!=nullptr; part=part->Next()) {
		if (part->cargo_cap != 0 && part->cargo_type < NUM_CARGO && IsCargoInClass(part->cargo_type,CargoClass::Passengers)) return true;
	}
	return false;
}

/** Equal-speed turnover is normally for distant arrivals. Break a near deadlock
 * only when the priority follower is physically stopped behind that arrival. */
bool EqualSpeedTurnover(const Area &area, const Train *v, const MainApproach &arrival, const std::vector<MainApproach> &approaches)
{
	{
		AutoYieldPreferenceBudget budget;
		const Train *front=arrival.train->GetMovingFront(), *departing=v->GetMovingFront();
		const auto incoming_distance=RouteToRole(area,arrival.train,front->tile,front->GetVehicleTrackdir(),AutoYieldRole::Clear,false);
		const auto departing_distance=RouteToRole(area,v,departing->tile,departing->GetVehicleTrackdir(),AutoYieldRole::Clear,false);
		if (incoming_distance && departing_distance && *incoming_distance > *departing_distance + EQUAL_SPEED_TURNOVER_GAP) return true;
	}
	if (v->cur_speed != 0 || arrival.train->cur_speed != 0) return false;
	for (const MainApproach &follower : approaches) {
		if (HasLaneClaim(area,follower.train) || StationPriorityPaused(area,follower.train) ||
				!FollowingThreat(area,follower,arrival) || !HigherPriority(area,follower.train,v,area.percent)) continue;
		if (StoppedQueueBehind(arrival.train,follower.train)) return true;
	}
	return false;
}

/** A read-only proposal. Actual departure still requires ordinary PBS reservation. */
struct EvacuationPlan {
	const Train *departing = nullptr;
	const Train *incoming = nullptr;
};
EvacuationPlan Evacuation(const Area &area, const std::vector<MainApproach> *known_approaches = nullptr, const Train *known_no_free_lane = nullptr)
{
	const std::vector<MainApproach> local_approaches = known_approaches == nullptr ? GetMainApproaches(area) : std::vector<MainApproach>{};
	const auto &approaches = known_approaches != nullptr ? *known_approaches : local_approaches;
	std::vector<MainApproach> incoming;
	std::vector<const Train *> overtaking_arrivals;
	for (const MainApproach &slow : approaches) {
		if (HasLaneClaim(area, slow.train)) continue;
		bool threatened = false, merging = false;
		for (const MainApproach &fast : approaches) {
			if (FollowingThreat(area, fast, slow, &merging)) threatened = true;
		}
		const bool overtaking = threatened && (slow.train == known_no_free_lane || merging || !FreeParallelMain(area, slow));
		if (overtaking) overtaking_arrivals.push_back(slow.train);
		/* The fastest train may itself require an occupied platform. It has
		 * no faster follower, so ordinary overtaking cannot free its station. */
		if (overtaking || (area.has_platform && slow.train->current_order.IsType(OT_GOTO_STATION))) incoming.push_back(slow);
	}
	SetApproachGaps(area,incoming,approaches);
	std::sort(incoming.begin(), incoming.end(), [&area](const MainApproach &a, const MainApproach &b) {
		return std::tuple{DetectionSpeed(area, a.train), HasPassengerPriority(a.train), a.priority_gap, a.distance, a.train->index} <
				std::tuple{DetectionSpeed(area, b.train), HasPassengerPriority(b.train), b.priority_gap, b.distance, b.train->index};
	});
	for (const MainApproach &arrival : incoming) {
		std::vector<TraceRestrictRefId> requested_platforms;
		if (area.has_platform && arrival.train->current_order.IsType(OT_GOTO_STATION)) {
			for (const auto &[ref, lane] : area.lanes) {
				if (ScheduledPlatformStop(arrival.train, GetPlatformInfo(area, arrival.train, ref))) requested_platforms.push_back(ref);
			}
		}
		const bool station_access = !requested_platforms.empty();
		if (!station_access && std::find(overtaking_arrivals.begin(), overtaking_arrivals.end(), arrival.train) == overtaking_arrivals.end()) continue;
		/* A free ordinary siding cannot satisfy a scheduled platform stop.
		 * Conversely, a genuinely free suitable platform needs no eviction. */
		if (station_access) {
			bool free_platform = false;
			for (TraceRestrictRefId ref : requested_platforms) if (FreeLane(area, arrival.train, false, false, ref)) { free_platform = true; break; }
			if (free_platform) continue;
		}
		/* Do not empty an occupied siding when this arrival can already use
		 * a genuinely free alternative. A closed/occupied approach is not free. */
		if (!station_access && arrival.train != known_no_free_lane && FreeLane(area, arrival.train)) continue;
		struct TurnoverCandidate {
			const Train *train;
			bool platform;
			bool passenger;
			bool station_demand;
		};
		/* Resolve the new same-speed rule first. Comparing platform priority
		 * only on ties inside the old passenger-first comparator would create
		 * cycles across three candidates with different speeds. */
		std::map<uint, TurnoverCandidate> candidates_by_speed;
		for (const auto &[ref, lane] : area.lanes) for (VehicleID id : lane->occupants) {
			const Train *v = GetTrain(id);
			/* Never propose a departure the current station/coupling order cannot
			 * perform, even if a legacy platform's geometry is not provable. */
			if (!Active(v) || !area.detection->IsOccupant(id) || DoingStationWork(v)) continue;
			/* A reserved platform belongs to a train still approaching its own
			 * scheduled stop until the station order has actually completed. */
			if (v->current_order.IsType(OT_GOTO_STATION) && ScheduledPlatformStop(v, GetPlatformInfo(area, v, ref))) continue;
			const bool for_station = station_access && std::find(requested_platforms.begin(), requested_platforms.end(), ref) != requested_platforms.end();
			if (station_access && !for_station) continue;
			if (!for_station && DetectionSpeed(area,v) < DetectionSpeed(area,arrival.train)) continue;
			if (!for_station && DetectionSpeed(area,v) == DetectionSpeed(area,arrival.train) && !EqualSpeedTurnover(area,v,arrival,approaches)) continue;
			/* A faster train already ahead of this arrival must finish passing
			 * first. Missing approaches include trains past Main but whose tail
			 * has not cleared; distant/budget-limited followers remain behind. */
			bool passing = false;
			for (VehicleID other_id : area.detection->occupants) {
				const Train *other = GetTrain(other_id);
				/* This arrival cannot pass before entering its occupied platform.
				 * Exempt only that exact requester, never another passing train. */
				if (for_station && other == arrival.train) continue;
				if (!Active(other) ||
						!HigherPriority(area, other, v, area.percent) || StationPriorityPaused(area, other)) continue;
				if (DetectionSpeed(area, other) == DetectionSpeed(area, v) && !ClosePassenger(area, other, v)) continue;
				/* A short priority stop must not be bypassed through the separate
				 * turnover exception while MustWait deliberately protects it. */
				if (ServingPlatform(area, other)) { passing = true; break; }
				if (HasLaneClaim(area, other)) continue;
				auto position = std::find_if(approaches.begin(), approaches.end(), [other](const MainApproach &a) { return a.train == other; });
				if (position == approaches.end()) { passing = true; break; }
				if (position->main && arrival.main && position->main == arrival.main && !position->clearance_only) {
					if (position->distance <= arrival.distance) { passing = true; break; }
				} else {
					const auto other_distance = MergeClearanceDistance(area, *position), arrival_distance = MergeClearanceDistance(area, arrival);
					if (!other_distance || !arrival_distance || *other_distance <= *arrival_distance) { passing = true; break; }
				}
			}
			if (passing) continue;
			if (!RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Clear, true)) {
				/* Once the front passes Clear it cannot reach that marker again.
				 * Keep the incoming train's turn while the departing body still
				 * spans it; the existing tail event owns release of the live claim. */
				bool clearing = false;
				for (const Train *part = v; part != nullptr && probe_budget_remaining > 0; part = part->Next()) {
					probe_budget_remaining--;
					if (AtRole(area, part->tile, part->GetVehicleTrackdir(), AutoYieldRole::Clear)) { clearing = true; break; }
				}
				if (!clearing) continue;
			}
			const Binding *binding = FindBinding(area, ref);
			if (binding == nullptr) continue;
			/* Prove this specific arrival can use this specific siding after v
			 * leaves. Ignore only v's physical occupancy and reservations in this
			 * hypothetical check; every other train and restriction still applies.
			 * Neither reservations nor slots are changed by this proposal. */
			const auto length = area.exit_only ? InferredSidingLength(area, arrival.train, *binding) :
					RouteToRole(area, arrival.train, GetTraceRestrictRefIdTileIndex(ref), binding->td, AutoYieldRole::Exit, true, std::nullopt, nullptr, v);
			if (!length || *length * TILE_SIZE < arrival.train->gcache.cached_total_length + TILE_SIZE) continue;
			if (!RouteToRole(area, arrival.train, arrival.train->GetMovingFront()->tile, arrival.train->GetMovingFront()->GetVehicleTrackdir(),
					area.exit_only ? AutoYieldRole::Exit : AutoYieldRole::Siding, true, ref, nullptr, v)) continue;
			/* Getting in is insufficient: this train must also be able to leave
			 * through the shared clearance route with its own rail compatibility. */
			if (!RouteToRole(area, arrival.train, GetTraceRestrictRefIdTileIndex(ref), binding->td,
					AutoYieldRole::Clear, true, std::nullopt, nullptr, v)) continue;
			const PlatformInfo platform = GetPlatformInfo(area, v, ref);
			bool station_demand = for_station;
			if (!station_demand && platform.marked && platform.known && platform.exit) {
				for (const MainApproach &following : approaches) {
					const Train *passenger = following.train;
					if (HasLaneClaim(area, passenger) || !PassengerService(passenger) ||
							!passenger->current_order.IsType(OT_GOTO_STATION)) continue;
					const bool stopping = std::any_of(platform.stations.begin(), platform.stations.end(), [passenger](StationID station) {
						return passenger->current_order.ShouldStopAtStation(passenger, station, false);
					});
					if (!stopping) continue;
					const Train *front = passenger->GetMovingFront();
					if (RouteToRole(area, passenger, front->tile, front->GetVehicleTrackdir(), AutoYieldRole::Exit, false, platform.exit)) {
						station_demand = true;
						break;
					}
				}
			}
			const TurnoverCandidate candidate{v, platform.marked, PassengerService(v), station_demand};
			auto [entry, inserted] = candidates_by_speed.try_emplace(DetectionSpeed(area, v), candidate);
			const auto rank = [](const TurnoverCandidate &c) {
				return std::tuple{!c.station_demand, !c.platform, !c.passenger, c.train->index};
			};
			if (!inserted && rank(candidate) < rank(entry->second)) entry->second = candidate;
		}
		/* Retain the existing passenger/fastest policy between speed groups.
		 * Only candidates that passed every station and route check participate. */
		const Train *best = nullptr, *best_platform = nullptr;
		std::tuple<bool, uint, VehicleID> best_rank;
		std::tuple<uint, bool, VehicleID> best_platform_rank;
		for (const auto &[speed, candidate] : candidates_by_speed) {
			/* A following scheduled passenger needs a platform, not an ordinary
			 * siding. Clear its fastest eligible platform occupant first; use
			 * passenger service only as a speed tie-break, never to skip work. */
			if (candidate.station_demand) {
				const auto rank = std::tuple{UINT_MAX - speed, !candidate.passenger, candidate.train->index};
				if (best_platform == nullptr || rank < best_platform_rank) {
					best_platform = candidate.train;
					best_platform_rank = rank;
				}
			}
			const auto rank = std::tuple{!candidate.passenger, UINT_MAX - speed, candidate.train->index};
			if (best == nullptr || rank < best_rank) {
				best = candidate.train;
				best_rank = rank;
			}
		}
		if (best_platform != nullptr) return {best_platform, arrival.train};
		if (best != nullptr) return {best, arrival.train};
		if (probe_budget_remaining == 0) return {};
	}
	return {};
}
void Acquire(TraceRestrictSlot *slot, const Train *v, const TraceRestrictProgramInput &input, TraceRestrictProgramResult &out)
{
	if (input.permitted_slot_operations & TRPISP_ACQUIRE_TEMP_STATE) {
		if (!slot->OccupyUsingTemporaryState(v->index, TraceRestrictSlotTemporaryState::GetCurrent())) out.flags |= TRPRF_WAIT_AT_PBS;
	} else if (input.permitted_slot_operations & (TRPISP_ACQUIRE | TRPISP_PBS_RES_END_ACQUIRE)) {
		if (!slot->Occupy(v)) out.flags |= TRPRF_WAIT_AT_PBS;
	}
}
} // namespace

bool IsAutoYieldInstruction(TraceRestrictItemType type)
{
	return (type >= TRIT_COND_AUTO_YIELD_FASTER && type <= TRIT_COND_AUTO_YIELD_LEAVE) || type == TRIT_AUTO_YIELD_AREA || type == TRIT_AUTO_YIELD_CLEAR;
}
void MigrateLegacyAutoYieldSpeedCaps(TraceRestrictProgram &program)
{
	for (auto iter : program.IterateInstructionsMutable()) {
		const auto item = iter.Instruction();
		if (item.GetType() == TRIT_AUTO_YIELD_AREA && item.GetAuxField() == to_underlying(AutoYieldRole::Approach)) {
			/* v1-v7 low bits were a removed detection distance, never a speed. */
			iter.SecondaryRef() &= 0xFFFF0000u;
		}
	}
}
void InvalidateAutoYieldAreas() { dirty = true; }
void ResetAutoYieldAreas() { areas.clear(); dirty = true; }

void AutoYieldSlotVacated(TraceRestrictSlotID slot_id, VehicleID vehicle)
{
	const TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(slot_id);
	if (slot == nullptr || slot->automatic_yield_area != 0) return;
	if (dirty) {
		/* Edits can invalidate cached pointers. Cancellation still releases its
		 * saved lane claims before the next registry rebuild. */
		for (TraceRestrictSlot *lane : TraceRestrictSlot::Iterate()) {
			if (lane->automatic_yield_area == slot_id.base() + 1 && lane->owner == slot->owner) lane->VacateByID(vehicle);
		}
		return;
	}
	auto area = areas.find({CompanyID{slot->owner}, slot_id.base()});
	if (area == areas.end()) return;
	if (area->second.pending != nullptr) area->second.pending->VacateByID(vehicle);
	for (const auto &[ref, lane] : area->second.lanes) lane->VacateByID(vehicle);
}

void UpdateAutoYieldSlotSignals(const TraceRestrictSlot *slot)
{
	if (dirty) return;
	const uint16_t id = slot->automatic_yield_area == 0 ? slot->index.base() : slot->automatic_yield_area - 1;
	auto area = areas.find({CompanyID{slot->owner}, id});
	if (area == areas.end()) return;
	for (const SignalReference &signal : area->second.signal_dependants) {
		if (IsValidTile(signal.tile) && IsPlainRailTile(signal.tile) && HasSignalOnTrack(signal.tile, signal.track)) {
			AddTrackToSignalBuffer(signal.tile, signal.track, GetTileOwner(signal.tile));
		}
	}
	UpdateSignalsInBuffer();
}

void UpdateAutoYieldAreas()
{
	areas.clear();
	std::map<AreaKey, std::vector<SignalReference>> condition_dependants;
	for (const auto &[ref, mapping] : _tracerestrictprogram_mapping) {
		const TileIndex tile = GetTraceRestrictRefIdTileIndex(ref);
		const Track track = GetTraceRestrictRefIdTrack(ref);
		if (!IsValidTile(tile) || !IsPlainRailTile(tile) || !HasSignalOnTrack(tile, track)) continue;
		Trackdir td = TrackToTrackdir(track);
		const bool directional = HasSignalOnTrackdir(tile, td) != HasSignalOnTrackdir(tile, ReverseTrackdir(td));
		if (!HasSignalOnTrackdir(tile, td)) td = ReverseTrackdir(td);
		const CompanyID owner{GetTileOwner(tile)};
		const TraceRestrictProgram *program = TraceRestrictProgram::Get(mapping.program_id);
		for (auto iter : program->IterateInstructions()) {
			const auto item = iter.Instruction();
			if (!IsAutoYieldInstruction(item.GetType())) continue;
			TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(item.GetValue());
			if (slot == nullptr || slot->automatic_yield_area != 0 || slot->vehicle_type != VehicleType::Train || !slot->IsUsableByOwner(owner)) continue;
			const AreaKey key{CompanyID{slot->owner}, slot->index.base()};
			condition_dependants[key].emplace_back(tile, track);
			if (item.IsConditional() || !directional) continue;
			/* Markers require one direction; conditions can be used on either side. */
			Area &area = areas[key];
			area.owner = CompanyID{slot->owner};
			area.id = slot->index.base();
			area.detection = slot;
			const AutoYieldRole role = item.GetType() == TRIT_AUTO_YIELD_CLEAR ? AutoYieldRole::Clear : static_cast<AutoYieldRole>(item.GetAuxField());
			const bool platform = IsAutoYieldLaneRole(role) && (iter.Secondary() & AUTO_YIELD_PLATFORM);
			area.bindings.push_back({ref, role, td, platform, role == AutoYieldRole::Approach ? uint16_t(iter.Secondary() & 0xFFFF) : uint16_t{0}, GetAutoYieldRegistration(iter.Secondary())});
			area.has_platform |= platform;
			if (role == AutoYieldRole::Approach) area.percent = std::max<uint16_t>(area.percent, GetAutoYieldPercent(iter.Secondary()));
		}
	}
	for (auto &entry : areas) {
		Area &area = entry.second;
		std::sort(area.bindings.begin(), area.bindings.end(), [](const Binding &a, const Binding &b) {
			return std::tie(a.ref, a.role, a.td) < std::tie(b.ref, b.role, b.td);
		});
	}
	for (auto &entry : condition_dependants) {
		auto &dependants = entry.second;
		std::sort(dependants.begin(), dependants.end());
		dependants.erase(std::unique(dependants.begin(), dependants.end()), dependants.end());
	}
	std::map<std::tuple<CompanyID, uint16_t, uint32_t>, TraceRestrictSlot *> resources;
	for (TraceRestrictSlot *slot : TraceRestrictSlot::Iterate()) {
		if (slot->automatic_yield_area == 0) continue;
		resources[{CompanyID{slot->owner}, slot->automatic_yield_area, slot->automatic_yield_signal}] = slot;
	}
	for (auto &[key, area] : areas) {
		std::set<uint32_t> refs;
		/* Zero is a valid user slot ID. Internal lanes store ID + 1 to keep
		 * the existing zero == ordinary slot representation on disk. */
		const uint16_t resource_area = area.id + 1;
		std::set<std::pair<AutoYieldRole, TraceRestrictRefId>> unique_bindings;
		std::set<TraceRestrictRefId> unique_signals;
		uint approach_count = 0, clear_count = 0, exit_count = 0, main_count = 0;
		bool valid_bindings = true;
		for (const Binding &b : area.bindings) {
			if (!unique_bindings.emplace(b.role, b.ref).second || !unique_signals.emplace(b.ref).second) valid_bindings = false;
			if (b.role == AutoYieldRole::Siding) refs.insert(b.ref);
			approach_count += b.role == AutoYieldRole::Approach;
			clear_count += b.role == AutoYieldRole::Clear;
			exit_count += b.role == AutoYieldRole::Exit;
			main_count += b.role == AutoYieldRole::Main;
		}
		area.main_count = main_count;
		area.exit_only = refs.empty();
		if (area.exit_only) for (const Binding &b : area.bindings) if (b.role == AutoYieldRole::Exit) refs.insert(b.ref);
		area.signal_dependants = std::move(condition_dependants[key]);
		/* Invalid/incomplete layouts are inert, including resource allocation. */
		if (!valid_bindings || approach_count == 0 || approach_count > MAX_APPROACH_MARKERS || clear_count != 1 || exit_count == 0 || main_count == 0 || main_count > MAX_MAIN_MARKERS || refs.empty() || refs.size() > MAX_SIDINGS) continue;
		size_t missing = !resources.contains({area.owner, resource_area, AUTO_YIELD_PENDING_REF});
		for (uint32_t ref : refs) missing += !resources.contains({area.owner, resource_area, ref});
		if (!TraceRestrictSlot::CanAllocateItem(missing)) continue;
		TraceRestrictSlot *&pending = resources[{area.owner, resource_area, AUTO_YIELD_PENDING_REF}];
		if (pending == nullptr) {
			pending = TraceRestrictSlot::Create(area.owner);
			pending->automatic_yield_area = resource_area;
			pending->automatic_yield_signal = AUTO_YIELD_PENDING_REF;
			pending->max_occupancy = UINT32_MAX;
			pending->name = "Automatic yielding pending " + std::to_string(area.id);
		}
		area.pending = pending;
		for (uint32_t ref : refs) {
			auto resource_key = std::make_tuple(area.owner, resource_area, ref);
			TraceRestrictSlot *&slot = resources[resource_key];
			if (slot == nullptr) {
				slot = TraceRestrictSlot::Create(area.owner);
				slot->automatic_yield_area = resource_area; slot->automatic_yield_signal = ref;
				slot->max_occupancy = 1;
				slot->name = "Automatic yielding " + std::to_string(area.id) + ":" + std::to_string(ref);
			}
			area.lanes[ref] = slot;
		}
		area.enabled = true;
	}
	std::vector<TraceRestrictSlot *> unused;
	for (const auto &[key, slot] : resources) {
		auto a = areas.find({std::get<0>(key), uint16_t(std::get<1>(key) - 1)});
		if (a == areas.end() || (a->second.pending != slot && !a->second.lanes.contains(std::get<2>(key)))) unused.push_back(slot);
	}
	for (TraceRestrictSlot *slot : unused) {
		if (slot->automatic_yield_signal == AUTO_YIELD_PENDING_REF) {
			if (TraceRestrictSlot *source = TraceRestrictSlot::GetIfValid(slot->automatic_yield_area - 1)) {
				const auto occupants = slot->occupants;
				for (VehicleID id : occupants) source->VacateByID(id);
			}
		}
		TraceRestrictRemoveSlotID(slot->index); delete slot;
	}
	dirty = false;
}

/** Version 1 stored a numeric area per company and hid its detection slot.
 * Reuse that exact slot (and its occupants), making it visible and replacing
 * every numeric operand with the saved slot ID. No vehicle identity is guessed. */
bool MigrateAutoYieldAreasToSlots()
{
	std::map<AreaKey, TraceRestrictSlot *> detection;
	std::vector<TraceRestrictSlot *> legacy;
	for (TraceRestrictSlot *slot : TraceRestrictSlot::Iterate()) {
		if (slot->automatic_yield_area == 0) continue;
		legacy.push_back(slot);
		if (slot->automatic_yield_signal == AUTO_YIELD_DETECTION_REF) {
			detection[{CompanyID{slot->owner}, slot->automatic_yield_area}] = slot;
		}
	}
	std::map<TraceRestrictProgramID, CompanyID> owners;
	for (const auto &[ref, mapping] : _tracerestrictprogram_mapping) {
		owners.try_emplace(mapping.program_id, CompanyID{GetTileOwner(GetTraceRestrictRefIdTileIndex(ref))});
	}
	for (uint8_t c = 0; c < MAX_COMPANIES; c++) {
		for (const auto &backup : _tracerestrict_backups[CompanyID{c}].programs) owners.try_emplace(backup.program_id, CompanyID{c});
	}
	for (TraceRestrictProgram *program : TraceRestrictProgram::Iterate()) {
		for (auto iter : program->IterateInstructionsMutable()) {
			auto item = iter.InstructionRef();
			if (!IsAutoYieldInstruction(item.GetType())) continue;
			auto owner = owners.find(program->index);
			if (owner == owners.end() || item.GetValue() == 0) {
				item.SetValue(INVALID_TRACE_RESTRICT_SLOT_ID);
				continue;
			}
			const AreaKey key{owner->second, item.GetValue()};
			TraceRestrictSlot *&slot = detection[key];
			if (slot == nullptr) {
				if (!TraceRestrictSlot::CanAllocateItem()) return false;
				slot = TraceRestrictSlot::Create(owner->second);
				slot->max_occupancy = UINT32_MAX;
				slot->name = "Automatic yielding " + std::to_string(key.first.base()) + ":" + std::to_string(key.second);
			}
			item.SetValue(slot->index);
		}
		if (program->Validate().Failed()) return false;
	}
	/* Rewrite lane keys before exposing the old detection slots. */
	for (TraceRestrictSlot *slot : legacy) {
		if (slot->automatic_yield_signal == AUTO_YIELD_DETECTION_REF) continue;
		auto source = detection.find({CompanyID{slot->owner}, slot->automatic_yield_area});
		if (source == detection.end() || source->second == nullptr) return false;
		slot->automatic_yield_area = source->second->index.base() + 1;
	}
	for (const auto &[key, slot] : detection) {
		slot->automatic_yield_area = 0;
		slot->name = "Automatic yielding " + std::to_string(key.first.base()) + ":" + std::to_string(key.second);
	}
	InvalidateAutoYieldAreas();
	return true;
}

void AutoYieldSettingsChanged()
{
	InvalidateAutoYieldAreas();
	UpdateAutoYieldAreas();
	if (!_settings_game.vehicle.automatic_yield) {
		for (auto &[key, area] : areas) {
			if (area.pending != nullptr) {
				const auto occupants = area.pending->occupants;
				for (VehicleID id : occupants) area.detection->VacateByID(id);
			}
			for (const auto &[ref, lane] : area.lanes) lane->Clear();
		}
	}
	for (const auto &[key, area] : areas) UpdateAutoYieldSlotSignals(area.detection);
}

void AutoYieldGameTick()
{
	if (dirty) UpdateAutoYieldAreas();
	if (!_settings_game.vehicle.automatic_yield) return;
	if ((_state_ticks.base() & 63) != 0) return;
	/* Reconcile changed ownership/topology and only inspect registered vehicles. */
	for (auto &[key, area] : areas) {
		if (!area.enabled || area.detection == nullptr || area.detection->occupants.empty()) continue;
		std::set<VehicleID> approaching;
		if (area.pending != nullptr && !area.pending->occupants.empty()) {
			/* Query each detector once, not once per train. The PBS owner and
			 * saved pending slot reconstruct the same answer on a joining client. */
			for (const Binding &binding : area.bindings) {
				if (binding.role != AutoYieldRole::Approach || binding.registration != AutoYieldRegistration::Reservation) continue;
				const TileIndex tile = GetTraceRestrictRefIdTileIndex(binding.ref);
				const Track track = GetTraceRestrictRefIdTrack(binding.ref);
				if (!(GetReservedTrackbits(tile) & TrackToTrackBits(track))) continue;
				if (const Train *owner = GetTrainForReservation(tile, track)) approaching.insert(owner->Primary()->index);
			}
		}
		/* A user slot can contain far more vehicles than the active-area limit.
		 * Reconcile a deterministic window, never copy/scan the entire slot.
		 * Derive it from the saved game tick so joining clients use the same window. */
		const auto &occupants = area.detection->occupants;
		const size_t count = std::min(occupants.size(), MAX_PARTICIPANTS);
		const size_t first = (((_state_ticks.base() >> 6) % occupants.size()) * MAX_PARTICIPANTS) % occupants.size();
		std::vector<VehicleID> ids;
		ids.reserve(count);
		for (size_t i = 0; i < count; i++) ids.push_back(occupants[(first + i) % occupants.size()]);
		for (VehicleID id : ids) {
			const Train *v = GetTrain(id);
			bool invalid = v == nullptr || !v->IsPrimaryVehicle() || v->IsVirtualOrCarried() || v->IsInDepot() || !IsValidTile(v->tile);
			/* Before physical entry, cancellation/rerouting must not leave a
			 * phantom contender. Once entered, only ordinary tail clearance or
			 * explicit slot cancellation ends membership. */
			invalid |= area.pending != nullptr && area.pending->IsOccupant(id) && !approaching.contains(id);
			if (invalid) {
				area.detection->VacateByID(id);
				/* A physical lane is still protected by PBS. Only invalid/deleted or
				 * departed records are released; no track reservations are touched. */
				for (const auto &[ref, lane] : area.lanes) lane->VacateByID(id);
			}
		}
	}
}

bool EvaluateAutoYield(const Train *v, uint16_t id, AutoYieldCondition condition, bool invert)
{
	AutoYieldProbeBudget budget;
	const Area *area = FindArea(v, id);
	if (!_settings_game.vehicle.automatic_yield || area == nullptr || !Active(v) || !area->detection->IsOccupant(v->index)) return false;
	bool result = false;
	switch (condition) {
		case AutoYieldCondition::Faster:
		case AutoYieldCondition::NeedsSiding: result = NeedsSiding(*area, v); break;
		case AutoYieldCondition::FreeSiding: result = FreeLane(*area, v); break;
		case AutoYieldCondition::MustWait: result = MustWait(*area, v); break;
		case AutoYieldCondition::Full: result = !FreeLane(*area, v); break;
		case AutoYieldCondition::Evacuate: result = Evacuation(*area).departing == v; break;
		case AutoYieldCondition::MayLeave: result = !MustWait(*area, v) || Evacuation(*area).departing == v; break;
	}
	return result != invert;
}

void ExecuteAutoYield(const Train *v, const TraceRestrictProgramInput &input, TraceRestrictProgramResult &out, uint16_t id, AutoYieldRole role, bool &ignore_pathfinder_penalty)
{
	AutoYieldProbeBudget budget;
	if (v == nullptr || !HasSignalOnTrackdir(input.tile, input.trackdir)) return;
	if (role == AutoYieldRole::Clear) {
		/* Cleanup must work even when the area is disabled, incomplete or over
		 * its participant budget. Vacating also releases this train's lanes. */
		TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(id);
		if (slot != nullptr && slot->automatic_yield_area == 0 && slot->vehicle_type == VehicleType::Train &&
				(input.permitted_slot_operations & TRPISP_RELEASE_BACK)) {
			slot->Vacate(v);
		}
		return;
	}
	if (!_settings_game.vehicle.automatic_yield || !Active(v)) return;
	const Area *area = FindArea(v, id);
	if (area == nullptr) return;
	if (role != AutoYieldRole::Approach && !area->detection->IsOccupant(v->index)) return;
	if (IsAutoYieldLaneRole(role) && area->has_platform && !HasLaneClaim(*area, v)) {
		const auto ref = MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir));
		bool ordinary_available = false;
		{
			AutoYieldPreferenceBudget preference_budget;
			const PlatformInfo platform = GetPlatformInfo(*area, v, ref);
			if (platform.marked && !RequiresPlatform(v, platform)) {
				ordinary_available = FreeLane(*area, v, false, true);
				if (!ordinary_available && platform.known) {
					/* Resolve legacy exit markers back to their entrance claim. A
					 * cost preference never grants admission: prove length, exclusive
					 * occupancy and a clear departure route before cancelling costs. */
					std::optional<TraceRestrictRefId> lane_ref;
					if (area->lanes.contains(ref)) {
						lane_ref = ref;
					} else if (role == AutoYieldRole::Exit) {
						for (const auto &[candidate_ref, lane] : area->lanes) {
							if (GetPlatformInfo(*area, v, candidate_ref).exit == ref) { lane_ref = candidate_ref; break; }
						}
					}
					if (lane_ref && FreeLane(*area, v, false, false, lane_ref) && NeedsSiding(*area, v)) ignore_pathfinder_penalty = true;
				}
			}
		}
		/* Soft preference only: retain platform fallback when ordinary sidings
		 * are occupied, unreachable, too short, or not provably available. */
		if (ordinary_available) {
			out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS;
			return;
		}
	}
	if (role == AutoYieldRole::Approach) {
		const Binding *binding = FindBinding(*area, MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir)));
		if (binding == nullptr) return;
		const auto mode = binding->registration;
		const auto permissions = input.permitted_slot_operations;
		const bool physical = permissions & (TRPISP_RELEASE_FRONT | TRPISP_RELEASE_BACK);
		if (physical && !(permissions & TRPISP_ACQUIRE_TEMP_STATE)) {
			const bool crossed = mode == AutoYieldRegistration::Front ? (permissions & TRPISP_RELEASE_FRONT) :
					mode == AutoYieldRegistration::Tail && (permissions & TRPISP_RELEASE_BACK);
			if (crossed) area->detection->Occupy(v);
			/* Physical passage confirms a reservation claim, but never reclaims
			 * cancelled membership in reservation mode. Tail mode waits for tail. */
			if (mode != AutoYieldRegistration::Tail || (permissions & TRPISP_RELEASE_BACK)) area->pending->VacateByID(v->index);
			return;
		}
		if (mode != AutoYieldRegistration::Reservation) {
			if (!area->detection->OccupyDryRun(v->index)) out.flags |= TRPRF_WAIT_AT_PBS;
			return;
		}
		const bool member = area->detection->IsOccupant(v->index);
		bool committed = input.permitted_slot_operations & TRPISP_PBS_RES_END_ACQUIRE;
		/* Some ordinary PBS entry callbacks run before attempting a reservation.
		 * They may confirm an existing owned path, never invent a successful one. */
		if (!committed && (input.permitted_slot_operations & TRPISP_ACQUIRE) &&
				(GetReservedTrackbits(input.tile) & TrackToTrackBits(TrackdirToTrack(input.trackdir)))) {
			const Train *owner = GetTrainForReservation(input.tile, TrackdirToTrack(input.trackdir));
			committed = owner != nullptr && owner->Primary() == v->Primary();
		}
		if (input.permitted_slot_operations & TRPISP_ACQUIRE_TEMP_STATE) {
			Acquire(area->detection, v, input, out);
			if (!member && area->detection->IsOccupant(v->index)) Acquire(area->pending, v, input, out);
		} else if (committed) {
			if (area->detection->Occupy(v)) {
				if (!member) area->pending->Occupy(v);
			} else out.flags |= TRPRF_WAIT_AT_PBS;
		} else if (!area->detection->OccupyDryRun(v->index)) out.flags |= TRPRF_WAIT_AT_PBS;
	} else if (role == AutoYieldRole::Main) {
		if (PrioritisePassengerExit(*area, v)) {
			out.flags |= TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
			return;
		}
		/* Keep the admission and evacuation decisions within the same bounded
		 * proof. Repeating the approach scan could spend the budget before
		 * rediscovering a departure which the siding's exit already permits. */
		const auto approaches = GetMainApproaches(*area, v);
		if (NeedsSiding(*area, v, &approaches)) {
			if (FreeLane(*area, v)) out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
			/* Reserving up to Main would already commit the straight branch.
			 * Keep the reservation endpoint before the fork while a selected
			 * departure clears, so the next retry can actually enter its siding. */
			else if (Evacuation(*area, &approaches, v).incoming == v) out.flags |= TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
		} else if (area->has_platform && v->current_order.IsType(OT_GOTO_STATION) && Evacuation(*area, &approaches).incoming == v) {
			/* Keep the station requester before the fork while its platform is
			 * being freed, just as for the existing overtaking turnover. */
			out.flags |= TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
		}
	} else if (role == AutoYieldRole::Siding) {
		auto lane = area->lanes.find(MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir)));
		if (lane == area->lanes.end()) return;
		if (!lane->second->occupants.empty() && !lane->second->IsOccupant(v->index)) { out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS; return; }
		Acquire(lane->second, v, input, out);
	} else if (role == AutoYieldRole::Exit) {
		if (area->exit_only) {
			const auto ref = MakeTraceRestrictRefId(input.tile, TrackdirToTrack(input.trackdir));
			auto lane = area->lanes.find(ref);
			const Binding *binding = FindBinding(*area, ref);
			if (lane == area->lanes.end() || binding == nullptr) return;
			const auto length = InferredSidingLength(*area, v, *binding);
			if (!length) return;
			if (*length * TILE_SIZE < v->gcache.cached_total_length + TILE_SIZE || !lane->second->OccupyDryRun(v->index)) {
				out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
				return;
			}
			/* End-position dry runs only test exclusivity; the existing PBS
			 * reservation-end callback commits the claim after success. */
			Acquire(lane->second, v, input, out);
		}
		if (MustWait(*area, v) && Evacuation(*area).departing != v) out.flags |= TRPRF_WAIT_AT_PBS;
	}
}
