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
};
struct Area {
	CompanyID owner;
	uint16_t id;
	uint16_t percent = 0; ///< Strictest configured approach threshold; areas without an approach remain disabled.
	std::vector<Binding> bindings;
	TraceRestrictSlot *detection = nullptr;
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
size_t probe_budget_remaining = 0;
struct PassengerPriorityEntry { const Train *train; bool loaded; };
std::array<PassengerPriorityEntry, MAX_PARTICIPANTS> passenger_priority_cache;
size_t passenger_priority_count = 0; // Only valid within one read-only evaluation.

class AutoYieldProbeBudget {
public:
	AutoYieldProbeBudget() : previous(probe_budget_remaining)
	{
		if (probe_depth == 0) {
			probe_budget_remaining = MAX_PROBE_NODES_PER_EVALUATION;
			passenger_priority_count = 0;
		}
	}
	~AutoYieldProbeBudget() { probe_budget_remaining = previous; }
private:
	size_t previous;
};

/** A soft preference must leave room for the authoritative exit/claim checks. */
class AutoYieldPreferenceBudget {
public:
	AutoYieldPreferenceBudget() : previous(probe_budget_remaining), allowance(std::min(probe_budget_remaining / 2, MAX_ROUTE_NODES))
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
	if (slot == nullptr || slot->automatic_yield_area != 0 || slot->vehicle_type != VehicleType::Train || !slot->IsUsableByOwner(v->owner)) return nullptr;
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
		if (part->cargo_type < NUM_CARGO && part->cargo.StoredCount() != 0 && IsCargoInClass(part->cargo_type, CargoClass::Passengers)) { loaded = true; break; }
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
			/* A platform is straight, but each skipped tile still needs its own
			 * occupancy/reservation check. Wormholes remain unproved. */
			if (free_path && follower.tiles_skipped != 0) {
				if (!follower.is_station || size_t(follower.tiles_skipped) > probe_budget_remaining) continue;
				bool clear = true;
				const TileIndexDiff step = TileOffsByDiagDir(follower.exitdir);
				for (int i = follower.tiles_skipped; i > 0; --i) {
					probe_budget_remaining--;
					if (!FreeTrackTile(v, follower.new_tile - step * i, FindFirstTrackdir(follower.new_td_bits), departing)) { clear = false; break; }
				}
				if (!clear) continue;
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
};
std::vector<MainApproach> GetMainApproaches(const Area &area)
{
	if (area.main_count == 0) return {};
	std::vector<MainApproach> result;
	result.reserve(area.detection->occupants.size());
	for (VehicleID id : area.detection->occupants) {
		const Train *train = GetTrain(id);
		if (!Active(train) || !area.detection->IsUsableByOwner(train->owner) || ServingPlatform(area, train)) continue;
		bool incomplete = false;
		std::optional<TraceRestrictRefId> main;
		auto distance = RouteToRole(area, train, train->GetMovingFront()->tile, train->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Main, false, std::nullopt, &incomplete, nullptr, &main);
		/* Registered members beyond the local route-proof budget remain
		 * potential followers. A computational cap must not become a hidden
		 * detection distance. Unknown membership never grants track access. */
		if (distance || incomplete) result.push_back({train, distance.value_or(UINT_MAX), main});
	}
	return result;
}
bool FreeLane(const Area &area, const Train *v, bool approaching = false, bool ordinary_only = false, std::optional<TraceRestrictRefId> exact_ref = std::nullopt);

/** Distances to different tracks cannot establish which train is following.
 * An unfinished proof retains the old conservative unknown-follower handling. */
bool FollowingThreat(const Area &area, const MainApproach &fast, const MainApproach &slow)
{
	return fast.train != slow.train && (!fast.main || !slow.main || fast.main == slow.main) &&
			HigherPriority(area, fast.train, slow.train, area.percent) && fast.distance > slow.distance;
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
	std::vector<MainApproach> approaches = known_approaches != nullptr ? *known_approaches : GetMainApproaches(area);
	for (const MainApproach &approach : approaches) if (approach.train == v) own = approach;
	if (own.distance == UINT_MAX) {
		auto distance = RouteToRole(area, v, v->GetMovingFront()->tile, v->GetMovingFront()->GetVehicleTrackdir(), AutoYieldRole::Main, false, std::nullopt, nullptr, nullptr, &own.main);
		if (!distance) return false;
		own.distance = *distance;
	}
	const auto threatened = [&](const MainApproach &slow) {
		for (const MainApproach &approach : approaches) {
			if (FollowingThreat(area, approach, slow)) return true;
		}
		return false;
	};
	if (!threatened(own) || FreeParallelMain(area, own)) return false;
	/* Allocate scarce sidings to the slowest reachable contenders first.
	 * Existing claims remain authoritative; no train is held on the main line
	 * merely to wait for a slower train to arrive. Equal speeds use distance,
	 * then the stable vehicle ID, never iteration order or random numbers. */
	if (HasLaneClaim(area, v)) return true;
	size_t available = 0;
	for (const auto &[ref, lane] : area.lanes) if (lane->occupants.empty()) available++;
	if (available == 0) return true; // Preserve the existing safe evacuation path.
	std::sort(approaches.begin(), approaches.end(), [&area](const MainApproach &a, const MainApproach &b) {
		return std::tuple{DetectionSpeed(area, a.train), HasPassengerPriority(a.train), a.distance, a.train->index} <
				std::tuple{DetectionSpeed(area, b.train), HasPassengerPriority(b.train), b.distance, b.train->index};
	});
	size_t priority = 0;
	for (const MainApproach &approach : approaches) {
		if (approach.train == v) break;
		if (DetectionSpeed(area, approach.train) > DetectionSpeed(area, v)) break;
		if (HasLaneClaim(area, approach.train) || !threatened(approach)) continue;
		if (!FreeParallelMain(area, approach) && FreeLane(area, approach.train, true) && ++priority >= available) return false;
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
		if (other != v && Active(other) && area.detection->IsUsableByOwner(other->owner) && HigherPriority(area, other, v, area.percent) && !ServingPlatform(area, other)) {
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
	for (const MainApproach &slow : approaches) {
		if (HasLaneClaim(area, slow.train)) continue;
		for (const MainApproach &fast : approaches) {
			if (FollowingThreat(area, fast, slow)) {
				/* Only threatened arrivals need the optional parallel proof.
				 * The caller may already have proved this arrival has no bypass. */
				if (slow.train == known_no_free_lane || !FreeParallelMain(area, slow)) incoming.push_back(slow);
				break;
			}
		}
	}
	std::sort(incoming.begin(), incoming.end(), [&area](const MainApproach &a, const MainApproach &b) {
		return std::tuple{DetectionSpeed(area, a.train), HasPassengerPriority(a.train), a.distance, a.train->index} <
				std::tuple{DetectionSpeed(area, b.train), HasPassengerPriority(b.train), b.distance, b.train->index};
	});
	for (const MainApproach &arrival : incoming) {
		/* Do not empty an occupied siding when this arrival can already use
		 * a genuinely free alternative. A closed/occupied approach is not free. */
		if (arrival.train != known_no_free_lane && FreeLane(area, arrival.train)) continue;
		const Train *best = nullptr;
		for (const auto &[ref, lane] : area.lanes) for (VehicleID id : lane->occupants) {
			const Train *v = GetTrain(id);
			/* Never propose a departure the current station/coupling order cannot
			 * perform, even if a legacy platform's geometry is not provable. */
			if (!Active(v) || !area.detection->IsOccupant(id) || !HigherPriority(area, v, arrival.train, 0) || (area.has_platform && DoingStationWork(v))) continue;
			/* A faster train already ahead of this arrival must finish passing
			 * first. Missing approaches include trains past Main but whose tail
			 * has not cleared; distant/budget-limited followers remain behind. */
			bool passing = false;
			for (VehicleID other_id : area.detection->occupants) {
				const Train *other = GetTrain(other_id);
				if (!Active(other) || !area.detection->IsUsableByOwner(other->owner) ||
						!HigherPriority(area, other, v, area.percent) || HasLaneClaim(area, other) || ServingPlatform(area, other)) continue;
				auto position = std::find_if(approaches.begin(), approaches.end(), [other](const MainApproach &a) { return a.train == other; });
				if (position == approaches.end() || !position->main || !arrival.main || position->main != arrival.main || position->distance <= arrival.distance) { passing = true; break; }
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
			if (best == nullptr || DetectionSpeed(area, v) > DetectionSpeed(area, best) ||
					(DetectionSpeed(area, v) == DetectionSpeed(area, best) &&
						std::tuple{!HasPassengerPriority(v), v->index} < std::tuple{!HasPassengerPriority(best), best->index})) best = v;
		}
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
			area.bindings.push_back({ref, role, td, platform, role == AutoYieldRole::Approach ? uint16_t(iter.Secondary() & 0xFFFF) : uint16_t{0}});
			area.has_platform |= platform;
			if (role == AutoYieldRole::Approach) area.percent = std::max<uint16_t>(area.percent, iter.Secondary() >> 16);
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
		size_t missing = 0;
		for (uint32_t ref : refs) missing += !resources.contains({area.owner, resource_area, ref});
		if (!TraceRestrictSlot::CanAllocateItem(missing)) continue;
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
		if (a == areas.end() || !a->second.lanes.contains(std::get<2>(key))) unused.push_back(slot);
	}
	for (TraceRestrictSlot *slot : unused) { TraceRestrictRemoveSlotID(slot->index); delete slot; }
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
		for (auto &[key, area] : areas) for (const auto &[ref, lane] : area.lanes) lane->Clear();
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
			bool invalid = v == nullptr || !v->IsPrimaryVehicle() || v->IsVirtualOrCarried() || v->IsInDepot() || !IsValidTile(v->tile) || !area.detection->IsUsableByOwner(v->owner);
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
				slot->IsUsableByOwner(v->owner) && (input.permitted_slot_operations & TRPISP_RELEASE_BACK)) {
			slot->Vacate(v);
		}
		return;
	}
	if (!_settings_game.vehicle.automatic_yield || !Active(v)) return;
	/* Physical detection is independent of downstream role validation and
	 * cache rebuild timing, just like tail clearance above. An invalid exit
	 * may disable yielding, but must not erase the section's membership. */
	if (role == AutoYieldRole::Approach && (input.permitted_slot_operations & TRPISP_RELEASE_FRONT) &&
			!(input.permitted_slot_operations & TRPISP_ACQUIRE_TEMP_STATE)) {
		TraceRestrictSlot *slot = TraceRestrictSlot::GetIfValid(id);
		if (slot != nullptr && slot->automatic_yield_area == 0 && slot->vehicle_type == VehicleType::Train && slot->IsUsableByOwner(v->owner)) slot->Occupy(v);
		return;
	}
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
		/* Detection is not a batch-entry gate: an occupied siding must not
		 * stop unregistered traffic before it can join the section. Keep source
		 * capacity protection; only physical front passage commits membership. */
		if (!area->detection->OccupyDryRun(v->index)) out.flags |= TRPRF_WAIT_AT_PBS;
	} else if (role == AutoYieldRole::Main) {
		/* Keep the admission and evacuation decisions within the same bounded
		 * proof. Repeating the approach scan could spend the budget before
		 * rediscovering a departure which the siding's exit already permits. */
		const auto approaches = GetMainApproaches(*area);
		if (NeedsSiding(*area, v, &approaches)) {
			if (FreeLane(*area, v)) out.flags |= TRPRF_DENY | TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
			/* Reserving up to Main would already commit the straight branch.
			 * Keep the reservation endpoint before the fork while a selected
			 * departure clears, so the next retry can actually enter its siding. */
			else if (Evacuation(*area, &approaches, v).incoming == v) out.flags |= TRPRF_WAIT_AT_PBS | TRPRF_PBS_RES_END_WAIT;
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
