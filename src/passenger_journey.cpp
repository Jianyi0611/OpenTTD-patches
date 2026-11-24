/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_journey.cpp Rail journey forecasts for passengers with a known destination. */
#include "stdafx.h"
#include "passenger_journey.h"
#include "train.h"
#include "roadveh.h"
#include "station_base.h"
#include "linkgraph/linkgraph.h"
#include "town.h"
#include "company_base.h"
#include "external_city.h"
#include "depot_base.h"
#include "economy_func.h"
#include "cargotype.h"
#include "timetable.h"
#include "date_func.h"
#include "settings_type.h"
#include "window_func.h"
#include <map>
#include <set>
#include "safeguards.h"

uint PassengerFareDistance(const Station &from, const Station &to)
{
	uint distance = DistanceManhattan(from.xy, to.xy);
	for (const Station *st : {&from, &to}) {
		if (st->IsExternalMarket()) {
			const auto *city = ExternalCity::GetIfValid(ExternalCityID{st->external_city});
			if (city != nullptr) distance += city->distance_tiles;
		}
	}
	return distance;
}

static int32_t ReferenceFare(uint distance)
{
	return ClampTo<int32_t>(GetTransportedGoodsIncome(1, distance, ClampTo<uint16_t>(distance * 16 / CARGO_AGING_TICKS), GetCargoTypeByLabel(CT_PASSENGERS)).base());
}

static bool UseRouteFares()
{
	return _settings_game.linkgraph.passenger_through_fares && _settings_game.linkgraph.passenger_route_fares;
}

/** Quote every operator on the same end-to-end distance, then weight its contribution by its share of the itinerary. */
struct ItineraryFare {
	uint journey_distance;
	uint64_t distance = 0;
	uint64_t weighted = 0;
	bool custom = false;
	std::vector<std::pair<StationID, uint64_t>> weights;
	int32_t reference = -1;

	void Add(const Vehicle &v, OrderListID plan_id, StationID stop, uint leg, int64_t duration, bool breakdown)
	{
		const auto &company = Company::Get(v.owner)->passenger_tariff;
		leg = std::max(1U, leg);
		uint64_t rate;
		const auto &plan = OrderList::Get(plan_id)->passenger_tariff;
		if (company.Active() || plan.Active() || v.passenger_tariff.Active()) {
			custom = true;
			/* Inverse of native straight rail motion: 2048 tick-km/h per tile. */
			const uint speed = duration > 0 ? ClampTo<uint>(uint64_t(leg) * 2048 / duration) : 0;
			rate = Company::Get(v.owner)->QuotePassengerFare(journey_distance, speed, v.passenger_tariff, &plan);
		} else {
			if (reference < 0) reference = ReferenceFare(journey_distance);
			rate = uint64_t(reference) * 100;
		}
		distance += leg;
		weighted += rate * leg;
		if (breakdown) weights.emplace_back(stop, rate * leg);
	}

	void AddRouteLeg(const Vehicle &v, OrderListID schedule, StationID stop, uint offset, uint length, int64_t duration, bool breakdown)
	{
		const auto *plan = &OrderList::Get(schedule)->passenger_tariff;
		const auto *company = Company::Get(v.owner);
		const uint speed = duration > 0 ? ClampTo<uint>(uint64_t(length) * 2048 / duration) : 0;
		const uint64_t cents = company->QuotePassengerFare(offset + length, speed, v.passenger_tariff, plan)
				- company->QuotePassengerFare(offset, speed, v.passenger_tariff, plan);
		weighted += cents;
		if (breakdown) weights.emplace_back(stop, cents);
	}

	int32_t Finish(std::vector<uint32_t> *shares, bool route = false) const
	{
		if (!route && (!custom || distance == 0)) return -1;
		const uint64_t divisor = route ? 100 : distance * 100;
		const auto total = ClampTo<int32_t>((weighted + divisor / 2) / divisor);
		if (shares != nullptr) {
			uint64_t cumulative = 0;
			uint32_t assigned = 0;
			for (auto [stop, weight] : weights) {
				cumulative += weight;
				const uint32_t upto = std::min<uint64_t>(total, (cumulative + divisor / 2) / divisor);
				shares->insert(shares->end(), {stop.base(), upto - assigned});
				assigned = upto;
			}
		}
		return total;
	}
};

int32_t PassengerServices::Quote(const RideSection &last, StationID origin, StationID destination, std::vector<uint32_t> *shares, uint32_t *distance) const
{
	auto previous = [&](const RideSection *r) { return r->previous ? &this->ride_sections[r->previous - 1] : nullptr; };
	if (UseRouteFares()) {
		uint64_t total = 0;
		for (auto leg = &last; leg; leg = previous(leg)) total += this->timetable->distances.Between(*leg->board, *leg->end_call);
		if (total > UINT32_MAX) return -1;
		if (distance) *distance = static_cast<uint32_t>(total);
		ItineraryFare fare{};
		for (auto leg = &last; leg; leg = previous(leg)) {
			const uint length = static_cast<uint>(this->timetable->distances.Between(*leg->board, *leg->end_call));
			total -= length;
			fare.AddRouteLeg(*Vehicle::Get(leg->train), leg->board->schedule, leg->end_call->station, static_cast<uint>(total), length,
					leg->board->timed && leg->end_call->timed ? leg->end_call->arrival - leg->board->departure : 0, shares != nullptr);
		}
		if (shares) std::ranges::reverse(fare.weights);
		return fare.Finish(shares, true);
	}
	ItineraryFare fare{PassengerFareDistance(*Station::Get(origin), *Station::Get(destination))};
	for (auto leg = &last; leg != nullptr; leg = leg->previous ? &this->ride_sections[leg->previous - 1] : nullptr) {
		fare.Add(*Vehicle::Get(leg->train), leg->board->schedule, leg->end_call->station, PassengerFareDistance(*Station::Get(leg->board->station), *Station::Get(leg->end_call->station)),
				leg->board->timed && leg->end_call->timed ? leg->end_call->arrival - leg->board->departure : 0, shares != nullptr);
	}
	if (shares) std::ranges::reverse(fare.weights);
	return fare.Finish(shares);
}

/** Resolve the passenger terminal of an order. Depot and station IDs use different pools. */
static StationID PassengerStop(const Order &order)
{
	if (order.IsType(OT_GOTO_STATION) && !order.IsExternalFlightOrder()) {
		if (order.GetNonStopType() & ONSF_NO_STOP_AT_DESTINATION_STATION) return StationID::Invalid();
		return order.GetDestination().ToStationID();
	}
	return GetExternalOrderStation(&order);
}

bool IsPassengerTerminal(const Station &station)
{
	return station.external_city != UINT16_MAX || station.facilities.Any({StationFacility::Train, StationFacility::BusStop});
}

/** Prefix distance is built in the existing forecast pass. Pricing a ride never walks the track network. */
struct RouteMileage {
	TileIndex previous = INVALID_TILE;
	uint64_t tiles = 0;

	void Arrive(const Order &order)
	{
		if (order.IsExternalTripOrder()) {
			const auto *city = ExternalCity::GetIfValid(GetExternalOrderCity(order));
			if (order.IsExternalFlightOrder()) {
				if (city && previous != INVALID_TILE) {
					const TileIndex radar = GetExternalFlightBoundary(city->index, previous);
					if (radar != INVALID_TILE) tiles += DistanceManhattan(previous, radar) + city->distance_tiles;
					previous = radar;
				}
				return;
			}
			const auto *depot = Depot::GetIfValid(order.GetDestination().ToDepotID());
			if (city && depot && previous != INVALID_TILE) tiles += DistanceManhattan(previous, depot->xy) + city->distance_tiles;
			const auto *back = Depot::GetIfValid(order.GetExternalReturnDepot());
			previous = back ? back->xy : depot ? depot->xy : INVALID_TILE;
		} else if (order.IsType(OT_GOTO_STATION) || order.IsType(OT_GOTO_WAYPOINT)) {
			if (const auto *at = BaseStation::GetIfValid(order.GetDestination().ToStationID())) {
				if (previous != INVALID_TILE) tiles += std::max(DistanceManhattan(previous, at->xy), order.FareDistanceFrom(previous, at->xy));
				previous = at->xy;
			}
		}
	}

	void Depart(const Order &order)
	{
		if (order.IsExternalTripOrder()) {
			if (const auto *city = ExternalCity::GetIfValid(GetExternalOrderCity(order))) tiles += city->distance_tiles;
		}
	}
};

/** Lazily build the mileage at each order once per shared plan and sale.
 * The final entry completes the trip back to order zero for rides spanning the cycle. */
uint64_t PassengerServices::RouteDistances::Between(const Call &board, const Call &end)
{
	auto [it, fresh] = this->plans.try_emplace(board.schedule);
	auto &prefix = it->second;
	if (fresh) {
		const auto &orders = OrderList::Get(board.schedule)->GetOrderVector();
		prefix.reserve(orders.size() + 1);
		RouteMileage route;
		/* Start at the last location, even when non-geographical orders straddle the boundary. */
		for (auto rev = orders.rbegin(); rev != orders.rend() && route.previous == INVALID_TILE; ++rev) {
			route.Arrive(*rev);
			route.Depart(*rev);
		}
		route.tiles = 0;
		for (const Order &order : orders) {
			route.Arrive(order);
			prefix.push_back(route.tiles);
			route.Depart(order);
		}
		prefix.push_back(route.tiles);
	}
	const uint64_t wrap = end.order < board.order ? prefix.back() : 0;
	return wrap + prefix[end.order] - prefix[board.order];
}

std::optional<uint32_t> PassengerBookedDistance(const Vehicle &v, VehicleOrderID call, StationID from, StationID to)
{
	if (call >= v.GetNumOrders() || PassengerStop(*v.GetOrder(call)) != from) return {};
	RouteMileage route;
	if (v.GetOrder(call)->IsExternalFlightOrder()) {
		/* A returning flight uses the gateway at which it left the map. */
		if (!v.IsInExternalTrip() || v.external_entry_tile == INVALID_TILE) return {};
		route.previous = v.external_entry_tile;
	}
	for (uint i = 0; i < v.GetNumOrders(); ++i) {
		const auto &order = *v.GetOrder((call + i) % v.GetNumOrders());
		if (order.IsType(OT_CONDITIONAL)) break;
		if (i != 0 || !order.IsExternalFlightOrder()) route.Arrive(order);
		if (i && PassengerStop(order) == to) return ClampTo<uint32_t>(route.tiles);
		if (i && PassengerStop(order) == from) break;
		route.Depart(order);
	}
	return {};
}

/** Quote one booked rail leg. External markets use the booked gateway, since their map label is only an anchor. */
std::optional<Money> EstimatePassengerLegFare(const Vehicle &train, VehicleOrderID call, const Station &from, const Station &to)
{
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	const uint orders = train.GetNumOrders();
	if (cargo == INVALID_CARGO || call >= orders || PassengerStop(*train.GetOrder(call)) != from.index) return std::nullopt;
	uint external_distance = 0;
	auto endpoint = [&](const Station &station, const Order &order, bool returning) -> TileIndex {
		if (!station.IsExternalMarket()) return station.xy;
		const ExternalCity *city = ExternalCity::GetIfValid(GetExternalOrderCity(order));
		if (city == nullptr || city->market_station != station.index) return INVALID_TILE;
		DepotID portal = order.GetDestination().ToDepotID();
		if (returning && order.GetExternalReturnDepot() != DepotID::Invalid()) portal = order.GetExternalReturnDepot();
		const Depot *depot = Depot::GetIfValid(portal);
		if (depot == nullptr || GetExternalCityForEntrance(depot->xy) != city->index) return INVALID_TILE;
		external_distance += city->distance_tiles;
		return depot->xy;
	};
	const TileIndex start = endpoint(from, *train.GetOrder(call), true);
	TileIndex end = to.xy;
	if (to.IsExternalMarket()) {
		end = INVALID_TILE;
		for (uint i = 1; i <= orders; ++i) {
			const Order *order = train.GetOrder((call + i) % orders);
			if (PassengerStop(*order) != to.index || order->GetCargoUnloadType(cargo) == OrderUnloadType::NoUnload) continue;
			end = endpoint(to, *order, false);
			break;
		}
	}
	if (UseRouteFares()) {
		const auto distance = PassengerBookedDistance(train, call, from.index, to.index);
		if (!distance) return {};
		const uint64_t cents = Company::Get(train.owner)->QuotePassengerFare(*distance, 0, train.passenger_tariff, &train.orders->passenger_tariff);
		return Money(ClampTo<int32_t>((cents + 50) / 100));
	}
	if (start == INVALID_TILE || end == INVALID_TILE) return std::nullopt;
	const uint distance = DistanceManhattan(start, end) + external_distance;
	/* Fixed reference pace (16 simulation ticks per tile), independent of timetable, number and traction. */
	const uint periods = distance * 16 / CARGO_AGING_TICKS;
	return GetTransportedGoodsIncome(1, distance, ClampTo<uint16_t>(periods), cargo);
}

bool IsPassengerBoardingAt(const Vehicle &v, StationID station)
{
	if (v.IsInExternalTrip()) {
		const ExternalCity *city = ExternalCity::GetIfValid(v.external_trip_city);
		return city && city->market_station == station && v.GetExternalTripPhase() == ExternalTripPhase::Stopped;
	}
	return v.current_order.IsAnyLoadingType() && v.last_station_visited == station;
}

/** The loading loop can finish one empty round while the timetable still
 * requires the train to stand. Sell this call only if another native loading
 * round can actually take place before its scheduled departure. */
bool PassengerLoadingOpen(const Vehicle &v)
{
	if (v.IsInExternalTrip()) return v.GetExternalTripPhase() == ExternalTripPhase::Stopped;
	if (v.type != VehicleType::Train) return !v.vehicle_flags.Test(VehicleFlag::LoadingFinished);
	if (v.vehicle_flags.Test(VehicleFlag::StopLoading)) return false;
	if (!v.vehicle_flags.Test(VehicleFlag::LoadingFinished)) return true;
	const TileIndex tile = Train::From(&v)->GetStationLoadingVehicle()->tile;
	return IsTileType(tile, TileType::Station) && GetStationIndex(tile) == v.last_station_visited && v.current_order.IsType(OT_LOADING) &&
			v.current_order.GetLeaveType() != OLT_LEAVE_EARLY &&
			int64_t(v.current_order_time) + v.load_unload_ticks < int64_t(v.current_order.GetTimetabledWait()) - v.lateness_counter;
}

/** Absolute section zero starts at the train's current real order; a seat is freed
 * before departure at the first legal alighting stop. Unknown walk-up rides are
 * conservatively counted through the next compulsory unload or one full circuit. */
static std::pair<uint, uint> SeatSections(const Vehicle &train, uint start, StationID alight, bool aboard)
{
	const uint n = train.GetNumOrders();
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	const uint limit = start + n;
	for (uint i = start + (aboard ? 0 : 1); i < limit; ++i) {
		const Order *order = train.GetOrder((train.cur_real_order_index + i) % n);
		const auto stop = PassengerStop(*order);
		if (stop == StationID::Invalid()) continue;
		const auto unload = order->GetCargoUnloadType(cargo);
		if (unload == OrderUnloadType::Transfer || unload == OrderUnloadType::Unload ||
				(unload != OrderUnloadType::NoUnload && stop == alight)) return {start, i};
	}
	return {start, limit};
}

static uint BoardSection(const Vehicle &train, VehicleOrderID call, uint8_t visit)
{
	const uint n = train.GetNumOrders();
	return (call + n - train.cur_real_order_index) % n + visit * n;
}

uint PassengerSeatInventory::Capacity(const Vehicle &train)
{
	auto [it, fresh] = this->capacities.try_emplace(train.index);
	if (fresh) {
		const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
		for (const Vehicle *unit = train.First(); unit != nullptr; unit = unit->Next()) {
			if (unit->cargo_type == cargo) it->second += unit->cargo_cap;
		}
	}
	return it->second;
}

static uint AllocateSeats(std::vector<uint32_t> &remaining, std::pair<uint, uint> range, uint requested)
{
	const auto [start, end] = range;
	if (end <= start || end > remaining.size()) return 0;
	const auto first = remaining.begin() + start, last = remaining.begin() + end;
	const uint room = *std::min_element(first, last);
	const uint take = std::min(room, requested);
	if (take > 0) for (auto it = first; it != last; ++it) *it -= take;
	return room;
}

PassengerSeatInventory::TrainSeats &PassengerSeatInventory::Get(VehicleID id)
{
	auto [it, fresh] = this->trains.try_emplace(id);
	auto &seats = it->second;
	if (!fresh) return seats;
	const Vehicle &train = *Vehicle::Get(id);
	const uint orders = train.GetNumOrders();
	if (orders == 0 || train.cur_real_order_index >= orders) return seats;
	uint free_at_start = this->Capacity(train);
	seats.remaining.resize(3 * orders);
	/* All passengers boarding and leaving at the same calls occupy the same
	 * sections. Timetable edits cannot occur during this inventory pass. */
	robin_hood::unordered_flat_map<uint64_t, uint> ends;
	auto sections = [&](uint start, StationID stop, bool aboard) {
		const uint64_t key = (uint64_t{start} << 17) | (uint64_t{stop.base()} << 1) | aboard;
		auto [cached, added] = ends.try_emplace(key);
		if (added) cached->second = SeatSections(train, start, stop, aboard).second;
		return std::pair{start, cached->second};
	};
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	const bool approaching = !train.current_order.IsAnyLoadingType() && train.GetExternalTripPhase() != ExternalTripPhase::Stopped;
	for (const Vehicle *unit = train.First(); unit != nullptr; unit = unit->Next()) {
		if (unit->cargo_type != cargo) continue;
		uint unloading = train.current_order.IsAnyLoadingType() ? unit->cargo.UnloadCount() : 0;
		for (const CargoPacket *packet : *unit->cargo.Packets()) {
			const uint leaving = std::min<uint>(unloading, packet->Count());
			unloading -= leaving;
			if (leaving == packet->Count()) continue;
			const auto stop = packet->passenger_alight != StationID::Invalid() ? packet->passenger_alight : packet->passenger_destination;
			const uint end = sections(0, stop, approaching).second;
			if (end == 0) continue;
			const uint used = std::min<uint>(free_at_start, packet->Count() - leaving);
			free_at_start -= used;
			seats.remaining[end] += used;
		}
	}
	/* Every passenger already aboard occupies a prefix starting at section zero.
	 * Release each seat once at its alighting call, including intermediate calls. */
	for (uint32_t &section : seats.remaining) {
		free_at_start += section;
		section = free_at_start;
	}
	auto sold = CargoPacket::TicketsForTrain(id);
	if (sold.empty()) return seats;
	seats.retained.reserve(sold.size());
	struct Booking { const CargoPacket *packet; std::pair<uint, uint> range; };
	std::vector<Booking> bookings;
	bookings.reserve(sold.size());
	std::vector<int64_t> changes(seats.remaining.size() + 1);
	for (const CargoPacket *packet : sold) {
		const auto &ticket = packet->ticket;
		seats.last_issue = std::max(seats.last_issue, ticket.issued);
		if (ticket.call >= orders || ticket.visit > 1 || !train.orders || ticket.schedule != train.orders->index) continue;
		const auto range = sections(BoardSection(train, ticket.call, ticket.visit), ticket.alight, false);
		bookings.push_back({packet, range});
		changes[range.first] += packet->Count();
		changes[range.second] -= packet->Count();
	}
	int64_t occupied = 0;
	for (size_t i = 0; i < seats.remaining.size(); ++i) {
		occupied += changes[i];
		changes[i] = occupied;
		if (occupied > seats.remaining[i]) seats.has_shortfall = true;
	}
	if (!seats.has_shortfall) {
		for (size_t i = 0; i < seats.remaining.size(); ++i) seats.remaining[i] -= changes[i];
		for (const auto &booking : bookings) seats.retained.emplace(booking.packet->index.base(), booking.packet->Count());
		return seats;
	}
	std::sort(bookings.begin(), bookings.end(), [](const Booking &a, const Booking &b) {
		return std::tie(a.packet->ticket.issued, a.packet->index) < std::tie(b.packet->ticket.issued, b.packet->index);
	});
	for (const auto &[packet, range] : bookings) {
		const uint room = AllocateSeats(seats.remaining, range, packet->Count());
		seats.retained.emplace(packet->index.base(), std::min<uint>(packet->Count(), room));
	}
	return seats;
}

/** During a station sale passengers stay on the platform, so removing a fully
 * accounted booking normally releases just its own sections. Rebuild if this
 * could promote a lower-priority booking, change the next issue number, or
 * if the train is actively loading and its onboard stock may have changed. */
bool PassengerSeatInventory::Cancel(const CargoPacket &packet, bool station_sale)
{
	const auto &ticket = packet.ticket;
	auto it = this->trains.find(ticket.train);
	if (it == this->trains.end()) return false;
	auto &seats = it->second;
	auto booked = seats.retained.find(packet.index.base());
	/* Choices only depend on whether a section has any room. Removing a
	 * stale booking from a section that already had seats cannot reopen a route. */
	bool reopened = false;
	if (booked != seats.retained.end() && booked->second > 0) {
		const Vehicle &train = *Vehicle::Get(ticket.train);
		const auto [start, end] = SeatSections(train, BoardSection(train, ticket.call, ticket.visit), ticket.alight, false);
		reopened = std::find(seats.remaining.begin() + start, seats.remaining.begin() + end, 0U) != seats.remaining.begin() + end;
	}
	if (station_sale && ticket.issued < seats.last_issue && booked != seats.retained.end() &&
			(booked->second == 0 || !seats.has_shortfall)) {
		if (booked->second > 0) {
			const Vehicle &train = *Vehicle::Get(ticket.train);
			const auto [start, end] = SeatSections(train, BoardSection(train, ticket.call, ticket.visit), ticket.alight, false);
			for (uint i = start; i < end; ++i) seats.remaining[i] += booked->second;
		}
		seats.retained.erase(booked);
	} else {
		this->trains.erase(it);
	}
	return reopened;
}

uint PassengerSeatInventory::Retained(const CargoPacket &packet)
{
	auto &retained = this->Get(packet.ticket.train).retained;
	auto it = retained.find(packet.index.base());
	return it == retained.end() ? 0 : it->second;
}

/** Keep the original reservation on the shared prefix, taking seats only on
 * the extra sections. A group that cannot all continue keeps its shorter tickets. */
CargoPacket *PassengerSeatInventory::ExtendReservation(CargoPacket &packet, StationID stop, uint start, uint end)
{
	auto &seats = this->Get(packet.ticket.train);
	const uint room = AllocateSeats(seats.remaining, {start, end}, 0);
	if (room == 0) return nullptr;
	CargoPacket *rest = nullptr;
	if (packet.Count() > room) {
		rest = packet.Split(packet.Count() - room);
		if (rest == nullptr) return nullptr;
		seats.retained[rest->index.base()] = rest->Count();
		seats.retained[packet.index.base()] = packet.Count();
	}
	AllocateSeats(seats.remaining, {start, end}, packet.Count());
	packet.ticket.alight = stop;
	return rest;
}

uint PassengerSeatInventory::Room(VehicleID id, VehicleOrderID call, uint8_t visit, StationID alight)
{
	const Vehicle &train = *Vehicle::Get(id);
	if (call >= train.GetNumOrders() || train.cur_real_order_index >= train.GetNumOrders()) return 0;
	return AllocateSeats(this->Get(id).remaining, SeatSections(train, BoardSection(train, call, visit), alight, false), 0);
}

static uint FindFirstFullSection(const std::vector<uint32_t> &remaining, uint start)
{
	if (start >= remaining.size()) return start;
	return std::find(remaining.begin() + start, remaining.end(), 0U) - remaining.begin();
}

uint PassengerSeatInventory::SectionRoom(VehicleID id, uint start, uint end)
{
	return AllocateSeats(this->Get(id).remaining, {start, end}, 0);
}

uint PassengerSeatInventory::FirstFullSection(VehicleID train, uint start)
{
	return FindFirstFullSection(this->Get(train).remaining, start);
}

uint PassengerSeatInventory::FirstKnownFullSection(VehicleID train, uint start) const
{
	const auto it = this->trains.find(train);
	return it == this->trains.end() ? UINT_MAX : FindFirstFullSection(it->second.remaining, start);
}

void PassengerSeatInventory::Use(VehicleID id, VehicleOrderID call, uint8_t visit, StationID alight, uint count)
{
	const Vehicle &train = *Vehicle::Get(id);
	AllocateSeats(this->Get(id).remaining, SeatSections(train, BoardSection(train, call, visit), alight, false), count);
}

/** A timetable can identify the next passenger working before the engine arrives.
 * Only project the automatic engine exchange whose continuing passenger coaches
 * all belong to one end of the train. A manual split may send seats to either plan. */
static bool ContinuesWithCoaches(const Train &train, const Order &marker, CargoType cargo)
{
	if (marker.GetNumDecouple() != 0 || marker.GetDecoupleSecondOrdersType() != ODOF_KEEP_ORDERS ||
			(marker.GetDecoupleFirstOrdersType() != ODOF_EXECUTE_SCHEDULE && marker.GetDecoupleFirstOrdersType() != ODOF_LOAD_AND_SCHEDULE)) return false;
	bool engines = false, wagons = false;
	uint transitions = 0;
	bool previous_engine = false;
	for (const Train *unit = train.First(); unit != nullptr; unit = unit->GetNextVehicle()) {
		const bool engine = Train::From(unit->GetUnitMain())->IsEngine();
		if ((engines || wagons) && previous_engine != engine) ++transitions;
		previous_engine = engine;
		engines |= engine;
		wagons |= !engine;
		if (engine) {
			for (const Train *part = unit; part != unit->GetNextVehicle(); part = part->Next()) {
				if (part->cargo_type == cargo && part->cargo_cap > 0) return false;
			}
		}
	}
	return wagons && transitions <= 1;
}

static bool IsPassengerAdministrativeOrder(const Order &order)
{
	return order.IsSlotCounterOrder() || order.IsType(OT_DUMMY) || order.IsType(OT_LABEL);
}

/** The passenger coaches retain this plan across an automatic engine exchange.
 * Pair the split and the wait on the same visit; other shunting commands may
 * divide passenger accommodation or send it onto another working. */
static bool IsThroughPassengerExchange(const Vehicle &v, VehicleOrderID index, CargoType cargo)
{
	if (v.type != VehicleType::Train) return false;
	const Order *marker = v.GetOrder(index);
	const bool split = marker->IsType(OT_DECOUPLE);
	if (!split && !marker->IsType(OT_WAIT_COUPLE)) return false;
	const uint count = v.GetNumOrders();
	for (uint i = 1; i < count; ++i) {
		const Order *other = v.GetOrder((index + (split ? i : count - i)) % count);
		if (IsPassengerAdministrativeOrder(*other)) continue;
		if (!other->IsType(split ? OT_WAIT_COUPLE : OT_DECOUPLE)) return false;
		return ContinuesWithCoaches(*Train::From(&v), split ? *marker : *other, cargo);
	}
	return false;
}

PassengerServices::Service PassengerServices::ForecastService(const Vehicle *vehicle, CargoType cargo)
{
	Service service{vehicle->index, {}};
	if (!vehicle->IsPrimaryVehicle() || vehicle->vehstatus.Any({VehState::Stopped, VehState::Crashed}) || vehicle->cur_real_order_index >= vehicle->GetNumOrders()) return service;
	bool seats = false;
	bool boarding_space = false;
	for (const Vehicle *unit = vehicle->First(); unit != nullptr; unit = unit->Next()) {
		if (unit->cargo_type != cargo || unit->cargo_cap == 0) continue;
		seats = true;
		if (unit->cargo.RemainingCount() < unit->cargo_cap || unit->cargo.UnloadCount() > 0) {
			boarding_space = true;
			break;
		}
	}
	if (!seats) return service;
	int64_t eta = 0;
	bool timed = true;
	int dispatch_schedule = -1, dispatch_slot = -1;
	const Order *number_change = nullptr;
	int64_t late = vehicle->lateness_counter;
	const auto count = vehicle->GetNumOrders();
	const OrderList *home = vehicle->IsExecutingSchedule() ? vehicle->GetPrimaryOrderList() : nullptr;
	const bool returns_home = home && home != vehicle->orders && home->GetNumOrders() != 0;
	service.calls.reserve(std::min<uint>(64, 2U * count + 1));
	/* Two cycles include the next departure and its onward calls across the order-list boundary. */
	for (uint i = 0; i <= 2U * count; ++i) {
		const VehicleOrderID order_index = (vehicle->cur_real_order_index + i) % count;
		if (returns_home && i != 0 && order_index == 0) break; // No second circuit on a temporary plan.
		const Order *order = vehicle->GetOrder(order_index);
		if (!order) break;
		if (IsPassengerAdministrativeOrder(*order)) continue;
		if (order->IsType(OT_WAIT_COUPLE) || order->IsType(OT_DECOUPLE)) {
			if (IsThroughPassengerExchange(*vehicle, order_index, cargo)) {
				timed = false; // The coaches continue, but the new engine's arrival time is unknown.
				continue;
			}
			if (order->IsType(OT_DECOUPLE) && (vehicle->type != VehicleType::Train || !ContinuesWithCoaches(*Train::From(vehicle), *order, cargo))) break;
			/* No through ride across a shunt. Sell the next passenger departure
			 * separately without inventing a time for an unconfirmed engine. */
			if (!service.calls.empty()) {
				service.calls.back().must_leave = true;
				service.calls.back().board = false;
			}
			timed = false;
			continue;
		}
		if (i == 0 && order->IsType(OT_GOTO_DEPOT) && vehicle->current_order.IsType(OT_WAITING) && !order->IsExternalTripOrder() &&
				!(order->GetDepotActionType() & ODATFB_HALT)) {
			eta += std::max<int64_t>(0, vehicle->current_order.GetTimetabledWait() - late - int64_t(vehicle->current_order_time));
			continue;
		}
		const bool external = order->IsExternalTripOrder();
		if (!external && !order->IsType(OT_GOTO_STATION) && !order->IsType(OT_GOTO_WAYPOINT)) break;
		const auto terminal = PassengerStop(*order);
		if (external && terminal == StationID::Invalid()) break;
		const bool here = i == 0 && IsPassengerBoardingAt(*vehicle, terminal);
		const bool approaching_city = i == 0 && external && vehicle->IsInExternalTrip() && vehicle->GetExternalTripPhase() == ExternalTripPhase::Outbound;
		const int64_t travel = order->GetTravelTime();
		if (approaching_city) {
			eta += std::max<int64_t>(0, (vehicle->external_arrival_tick - _state_ticks).base());
		} else if (!here) {
			/* Untimetabled orders retain the game's measured travel time after a completed run. */
			if (travel == 0) {
				/* Consecutive calls on the same rail platform can start loading again without a journey. */
				const bool stays_at_station = !service.calls.empty() && service.calls.back().section + 1 == i &&
						service.calls.back().station == terminal && vehicle->type == VehicleType::Train && !external &&
						Company::Get(vehicle->owner)->settings.remain_if_next_order_same_station;
				if (!stays_at_station) timed = false;
			}
			const int64_t elapsed = i == 0 ? vehicle->current_order_time : 0;
			if (travel > 0 && elapsed > 2 * travel) break; // A severely overdue approach has no reliable ETA.
			eta += std::max<int64_t>(1, travel - elapsed);
		}
		/* A train already loading has applied the stop's number, including any subsequent split. */
		if (!here && order->IsServiceNumberChangePoint() && order->GetServiceNumberMode() != 0) number_change = order;
		if (terminal == StationID::Invalid()) continue;
		const Station *station = Station::GetIfValid(terminal);
		if (!station) break;
		const int64_t arrival = eta;
		const auto unload = order->GetCargoUnloadType(cargo);
		int64_t wait = order->GetWaitTime();
		if (order->IsWaitTimetabled()) {
			const int64_t recovered = std::min(late, wait);
			wait -= recovered;
			late -= recovered;
		}
		if (external && (here || approaching_city)) {
			eta = std::max<int64_t>(eta, (vehicle->external_departure_tick - _state_ticks).base());
		} else {
			if (external && !order->IsWaitTimetabled()) {
				const auto *city = ExternalCity::Get(GetExternalOrderCity(*order));
				wait = int64_t(city->stop_minutes) * std::max<uint16_t>(1, _settings_game.game_time.ticks_per_minute);
			}
			eta += std::max<int64_t>(0, wait - (here ? vehicle->current_order_time : 0));
		}
		/* The current stop already holds its dispatch slot; its lateness includes the wait. */
		if (!here && vehicle->vehicle_flags.Test(VehicleFlag::ScheduledDispatch) && order->IsScheduledDispatchOrder(true)) {
			const auto &schedule = vehicle->orders->GetDispatchScheduleByIndex(order->GetDispatchScheduleIndex());
			if (schedule.GetScheduledDispatchDuration() == 0) break;
			if (!timed) {
				/* A fixed public number survives an unknown connection time. Slot-
				 * specific numbers can only be advertised after a slot is known. */
				if (number_change == nullptr || number_change->GetServiceNumberMode() != 1) break;
			} else {
				auto [slot, index] = GetScheduledDispatchTime(schedule, _state_ticks + eta);
				if (index < 0) break;
				dispatch_schedule = order->GetDispatchScheduleIndex();
				dispatch_slot = index;
				eta = std::max<int64_t>(eta, (slot - _state_ticks).base());
				late = 0;
			}
		}
		bool board = (!here || (boarding_space && PassengerLoadingOpen(*vehicle))) && order->GetCargoLoadType(cargo) != OrderLoadType::NoLoad &&
				(external || station->owner != OWNER_NONE || station->town->exclusive_counter == 0 || station->town->exclusivity == vehicle->owner);
		service.calls.push_back({station->index, arrival, eta, board, unload != OrderUnloadType::NoUnload,
				unload == OrderUnloadType::Transfer || unload == OrderUnloadType::Unload, here, unload != OrderUnloadType::Transfer, order_index, dispatch_schedule, dispatch_slot, timed, static_cast<uint8_t>(i / count),
				vehicle->type == VehicleType::Road && !order->IsWaitTimetabled() && !order->IsScheduledDispatchOrder(true) ? order->GetBusHeadway() : 0, i, vehicle->orders->index, number_change});
		if (IsFullLoadOrderLoadType(order->GetCargoLoadType(cargo))) timed = false; // The stops remain known, but the departure time is open-ended.
	}
	return service;
}

/** Expected departure for this ticket only; never borrow a time from another visit. */
std::optional<int64_t> PassengerServices::DepartureDelay(const Train *train, StationID station, VehicleOrderID order, uint8_t visit)
{
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	if (train == nullptr || cargo == INVALID_CARGO || order >= train->GetNumOrders()) return std::nullopt;
	const Order *boarding = train->GetOrder(order);
	if (PassengerStop(*boarding) != station || IsFullLoadOrderLoadType(boarding->GetCargoLoadType(cargo))) return std::nullopt;
	for (const Call &call : ForecastService(train, cargo).calls) {
		if (call.station == station && call.order == order && call.visit == visit && call.board && call.timed) return call.departure;
	}
	return std::nullopt;
}

PassengerServices::PassengerServices(PassengerSeatInventory *inventory) : inventory(inventory), timetable(std::make_shared<Timetable>())
{
}

/** Index shared orders once per sale. Forecast individual vehicles only when the
 * journey search actually reaches one of their stops. Both seat searches use
 * the same calls, but keep their own route labels and availability decisions. */
PassengerServices::Timetable::Timetable() : lines(Station::GetPoolSize()), boardings(lines.size()), expanded(OrderList::GetPoolSize()), indexed_stations(lines.size())
{
	for (const OrderList *list : OrderList::Iterate()) {
		const Vehicle *front = list->GetFirstSharedVehicle();
		if (!front || (front->type != VehicleType::Train && front->type != VehicleType::Road)) continue;
		for (const Order *order : list->Orders()) {
			const auto id = PassengerStop(*order);
			if (id.base() >= this->lines.size()) continue;
			auto &at = this->lines[id.base()];
			if (at.empty() || at.back() != list->index) at.push_back(list->index);
		}
	}
}

const std::vector<PassengerServices::BoardingCall> &PassengerServices::Timetable::At(StationID station)
{
	if (this->indexed_stations[station.base()]) return this->boardings[station.base()];
	this->indexed_stations[station.base()] = true;
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	for (OrderListID id : this->lines[station.base()]) {
		if (this->expanded[id.base()]) continue;
		this->expanded[id.base()] = true;
		for (const Vehicle *v = OrderList::Get(id)->GetFirstSharedVehicle(); v; v = v->NextShared()) {
			if (v->type == VehicleType::Road && (!v->IsPrimaryVehicle() || !RoadVehicle::From(v)->IsBus())) continue;
			Service service = ForecastService(v, cargo);
			service.priced = _settings_game.linkgraph.passenger_through_fares && (UseRouteFares() || v->passenger_tariff.Active() || v->orders->passenger_tariff.Active() || Company::Get(v->owner)->passenger_tariff.Active());
			if (service.calls.size() < 2) continue;
			const size_t index = this->services.size();
			for (size_t i = 0; i + 1 < service.calls.size(); ++i) {
				const Call &call = service.calls[i];
				if (call.board) this->boardings[call.station.base()].push_back({index, i});
			}
			this->services.push_back(std::move(service));
		}
	}
	return this->boardings[station.base()];
}

size_t PassengerServices::BoardingCall::NextVisit(const std::vector<Call> &calls) const
{
	if (this->next_visit == SIZE_MAX) {
		const auto order = calls[this->call].order;
		const auto again = std::find_if(calls.begin() + this->call + 1, calls.end(), [order](const Call &c) { return c.order == order && c.board; });
		this->next_visit = again - calls.begin();
	}
	return this->next_visit;
}

uint64_t PassengerServices::RouteLength(size_t route) const
{
	uint64_t total = 0;
	for (size_t i = route; i != 0;) {
		const auto &leg = this->ride_sections[i - 1];
		if (leg.total_distance != UINT64_MAX) {
			total += leg.total_distance;
			break;
		}
		total += this->timetable->distances.Between(*leg.board, *leg.end_call);
		i = leg.previous;
	}
	this->ride_sections[route - 1].total_distance = total;
	return total;
}

/** Expand one train ride per round. For the same arriving train an earlier arrival
 * dominates a later one, but arrival on a different train may enable a connection. */
const PassengerServices::Connection *PassengerServices::FindConnection(StationID from, StationID to, bool interchange_only, bool held, Query *query) const
{
	/* Intermediate stops still participate in every connection. Only compare
	 * complete itineraries at destinations requested in this station sale. */
	const std::vector<Demand> *buyers = nullptr;
	if (!interchange_only && _settings_game.linkgraph.passenger_through_fares) {
		auto [it, fresh] = this->waiting_destinations.try_emplace(from);
		auto &wanted = it->second;
		if (fresh) {
			wanted.resize(this->timetable->lines.size());
			const auto cargo = GetCargoTypeByLabel(CT_PASSENGERS);
			for (const auto &[via, packets] : *Station::Get(from)->goods[cargo].ConstCargoList().Packets()) {
				for (const CargoPacket *packet : packets) {
					const auto id = packet->passenger_destination.base();
					if (id < wanted.size()) wanted[id] = std::max(wanted[id], packet->through_fare < 0 ? Demand::NeedsQuote : Demand::Prepaid);
				}
			}
		}
		const Demand request = held ? Demand::Prepaid : Demand::NeedsQuote;
		if (wanted[to.base()] < request) {
			wanted[to.base()] = request;
			if (!fresh) this->ClearConnections();
		}
		buyers = &wanted;
	}
	auto [cache, inserted] = this->connections.try_emplace(std::pair{from, interchange_only});
	auto &destinations = cache->second;
	auto first_leg = [](const Connection &c) { return std::pair{c.train, c.alight}; };
	auto time_rank = [](const Connection &c) { return std::tuple{c.arrival, c.board->departure, c.train, c.alight}; };
	auto unknown_rank = [](const Connection &c) { return std::tuple{c.stops, c.board->visit, c.board->departure, c.train, c.alight}; };
	if (inserted) {
		/* Allow time to leave the arriving train and reach the connecting platform.
		 * Both forecasts are offsets from the same simulation tick. */
		const int64_t ticks_per_minute = std::max<uint16_t>(1, _settings_game.game_time.ticks_per_minute);
		const int64_t minimum_connection = 10 * ticks_per_minute;
		/* Keep the fastest two distinct first legs per ride count. The runner-up
		 * is used by the station tooltip. A shorter forecast with more changes
		 * must reach the final comparison even when a direct train exists. */
		struct Options {
			std::array<Connection, 2> timed{};
			Connection untimed{};
			std::vector<Connection> offers;
			std::vector<Connection> nearby_transfers;
			std::unique_ptr<std::array<Connection, 2>> ordinary;
		};
		const size_t count = this->timetable->lines.size();
		std::vector<std::vector<std::pair<uint, Options>>> options(count);
		/* Timed transfers that differ by only a few minutes can compete on mileage. */
		auto close_arrival = [&](int64_t earliest) { return earliest + std::max(3 * ticks_per_minute, earliest / 20); };
		auto record = [&](StationID station, const Connection &c, const RideSection &ride, bool priced) {
			/* Most candidates arrive in ride-count order. An earlier feeder retained at
			 * an interchange can also be revisited during a later round. */
			auto &rounds = options[station.base()];
			if (rounds.empty() || rounds.back().first < c.rides) rounds.emplace_back(c.rides, Options{});
			auto tier = std::prev(rounds.end());
			if (tier->first != c.rides) {
				tier = std::lower_bound(rounds.begin(), tier, c.rides, [](const auto &entry, uint n) { return entry.first < n; });
				if (tier->first != c.rides) tier = rounds.emplace(tier, c.rides, Options{});
			}
			auto &by_ride = tier->second;
			bool keep_price = false;
			if (buyers && (*buyers)[station.base()] == Demand::NeedsQuote) {
				const auto &fastest = by_ride.timed[0];
				const auto &shortest = by_ride.untimed;
				keep_price = c.arrival >= 0 ?
						!fastest.board || c.arrival <= fastest.arrival + std::max(minimum_connection, fastest.arrival / 5) :
						!shortest.board || std::tie(c.stops, c.board->visit) <= std::tie(shortest.stops, shortest.board->visit);
				if (keep_price && priced) {
					by_ride.offers.push_back(c);
				} else if (keep_price) {
					const bool timed = c.arrival >= 0;
					if (!by_ride.ordinary) by_ride.ordinary = std::make_unique<std::array<Connection, 2>>();
					auto &ordinary = (*by_ride.ordinary)[timed];
					keep_price = !ordinary.board || (timed ? time_rank(c) < time_rank(ordinary) : unknown_rank(c) < unknown_rank(ordinary));
					if (keep_price) ordinary = c;
				}
			}
			/* Runs on the same plan can share their entire measured path. There is
			 * no distance comparison to do for another occurrence of that path. */
			auto same_path = [&](const Connection &other) {
				const RideSection &leg = this->ride_sections[other.route - 1];
				return leg.previous == ride.previous && leg.board->schedule == ride.board->schedule &&
						leg.board->order == ride.board->order && leg.end_call->order == ride.end_call->order;
			};
			if (c.arrival < 0) {
				auto &other = by_ride.untimed;
				const bool earlier = !other.board || (c.rides > 1 && c.board->visit != other.board->visit ?
						c.board->visit < other.board->visit : unknown_rank(c) < unknown_rank(other));
				if (other.board && c.rides > 1 && c.board->visit == other.board->visit && !same_path(other)) {
					by_ride.nearby_transfers.push_back(earlier ? other : c);
					if (!earlier) return true;
				}
				if (!earlier) return keep_price;
				other = c;
				return true;
			}
			auto &pair = by_ride.timed;
			bool keep_distance = false;
			if (c.rides > 1 && pair[0].board) {
				const bool earlier = time_rank(c) < time_rank(pair[0]);
				const Connection &later = earlier ? pair[0] : c;
				if (later.arrival <= close_arrival(earlier ? c.arrival : pair[0].arrival) && !same_path(pair[0])) {
					by_ride.nearby_transfers.push_back(later);
					keep_distance = !earlier;
				}
			}
			auto &slot = !pair[0].board || first_leg(c) == first_leg(pair[0]) ? pair[0] : pair[1];
			if (slot.board && time_rank(c) >= time_rank(slot)) return keep_price || keep_distance;
			slot = c;
			if (pair[1].board && time_rank(pair[1]) < time_rank(pair[0])) std::swap(pair[0], pair[1]);
			return true;
		};
		struct Label {
			VehicleID last = VehicleID::Invalid();
			int64_t time = -1;
			VehicleID first = VehicleID::Invalid();
			const Call *board = nullptr;
			StationID alight = StationID::Invalid();
			uint stops = 0, rides = 0;
			size_t route = 0;
			uint32_t bus_margin = 0;
			bool priced = false;
		};
		auto improves = [](int64_t time, int64_t old) { return time >= 0 && (old < 0 || time < old); };
		using Arrivals = std::array<Label, 2>;
		/* A search for the first interchange stops after one vehicle. It has
		 * no next round to seed, even at a station with thousands of buyers. */
		std::vector<Arrivals> arrivals(interchange_only ? 0 : count);
		std::vector<bool> queued(interchange_only ? 0 : count);
		std::vector<std::pair<StationID, Arrivals>> frontier{{from, {}}};
		std::vector<StationID> next;
		for (uint rides = 1; !frontier.empty(); ++rides) {
			for (const auto &[station, labels] : frontier) {
				if (rides > 1 && Station::Get(station)->external_city != UINT16_MAX) continue; // The city terminal has no transfer platform.
				std::optional<bool> second_feeder_shorter;
				for (const auto &boarding : this->timetable->At(station)) {
					const size_t i = boarding.call;
					const Service &service = this->timetable->services[boarding.service];
					const Call &board = service.calls[i];
					if (query && rides == 1 && query->earliest > 0 && (!board.timed || board.departure < query->earliest)) continue;
					const Label *feeder = labels[0].last == service.train ? &labels[1] : &labels[0];
					/* Two feeders can catch the very same departure. An earlier arrival at
					 * the interchange alone need not justify the longer ride. */
					if (feeder == &labels[0] && labels[1].board && labels[1].last != service.train && board.timed && !board.headway) {
						auto can_connect = [&](const Label &l) {
							return l.time >= 0 && l.time + minimum_connection + l.bus_margin <= board.departure;
						};
						if (can_connect(labels[1])) {
							if (!can_connect(*feeder) || labels[1].rides < feeder->rides) {
								feeder = &labels[1];
							} else if (labels[1].rides == feeder->rides) {
								if (!second_feeder_shorter.has_value()) second_feeder_shorter = this->RouteLength(labels[1].route) < this->RouteLength(feeder->route);
								if (*second_feeder_shorter) feeder = &labels[1];
							}
						}
					}
					const Label &incoming = *feeder;
					if (rides > 1 && incoming.board == nullptr) continue;
					const auto &calls = service.calls;
					const bool timed = board.timed && (rides == 1 || incoming.time >= 0);
					const int64_t ready = rides == 1 ? 0 : incoming.time + minimum_connection + incoming.bus_margin;
					if (rides > 1 && timed && board.departure < ready) continue;
					const int64_t bus_wait = !board.boarding_now && board.headway > 0 ?
							std::max<int64_t>(0, ready + board.headway / 2 - board.departure) : 0;
					size_t repeat = i;
					size_t next_query_arrival = i + 1;
					const uint available_until = this->inventory ? this->inventory->FirstKnownFullSection(service.train, board.section) : UINT_MAX;
					uint repeat_available_until = UINT_MAX;
					for (size_t j = i + 1; j < calls.size(); ++j) {
						const Call &stop = calls[j];
						if (stop.station == station || stop.station == from) break;
						if (stop.alight) {
							const int64_t time = timed && stop.timed ? stop.arrival + bus_wait : -1;
							if (rides > 1 && time < 0) {
								if (repeat == i) {
									repeat = boarding.NextVisit(calls);
									if (this->inventory && repeat < calls.size()) repeat_available_until = this->inventory->FirstKnownFullSection(service.train, calls[repeat].section);
								}
								if (repeat + j - i >= calls.size() || calls[repeat + j - i].order != stop.order ||
										calls[repeat + j - i].section > repeat_available_until) break;
							} else if (stop.section > available_until) {
								/* Alighting at the first full section's origin still fits. A later untimed
								 * transfer can use the next occurrence, so keep examining those stops. */
								if (stop.must_leave) break;
								continue;
							}
							const auto offset = rides > 1 && time < 0 ? repeat - i : 0;
							bool retain_section = false;
							const Label label{service.train, time, rides == 1 ? service.train : incoming.first,
									rides == 1 ? &board : incoming.board, rides == 1 ? stop.station : incoming.alight,
									static_cast<uint>(j - i) + incoming.stops, 1 + incoming.rides, this->ride_sections.size() + 1, board.headway / 4, incoming.priced || service.priced};
							const RideSection ride{service.train, calls[i + offset].section, calls[j + offset].section, incoming.route, &board, &stop, label.priced};
							if (interchange_only || (stop.deliver && (!query || stop.station == to) && (!buyers || (*buyers)[stop.station.base()] != Demand::None))) {
								Connection c{label.first, label.alight, time, label.board, label.rides, label.stops, label.route};
								retain_section = record(stop.station, c, ride, label.priced && !interchange_only);
								if (query && stop.station == to && j >= next_query_arrival) {
									query->routes.push_back(label.route);
									retain_section = true;
									/* A train can execute several stop orders without leaving the platform.
									 * Advertise the first permitted arrival for this boarding only. */
									const Vehicle *v = Vehicle::Get(service.train);
									if (v->type == VehicleType::Train && v->GetOrder(stop.order)->IsType(OT_GOTO_STATION) &&
											Company::Get(v->owner)->settings.remain_if_next_order_same_station) {
										next_query_arrival = j + 1;
										while (next_query_arrival < calls.size()) {
											const auto &next = calls[next_query_arrival];
											const auto &prev = calls[next_query_arrival - 1];
											if (next.station != to || next.section != prev.section + 1 ||
													!v->GetOrder(next.order)->IsType(OT_GOTO_STATION)) break;
											++next_query_arrival;
										}
									}
								}
							}
							if (!interchange_only) {
								auto &pair = arrivals[stop.station.base()];
								auto &slot = pair[0].last == service.train || pair[0].board == nullptr ? pair[0] : pair[1];
								if (slot.board == nullptr || improves(time, slot.time)) {
									retain_section = true;
									slot = label;
									if (pair[1].board != nullptr && improves(pair[1].time, pair[0].time)) std::swap(pair[0], pair[1]);
									if (!queued[stop.station.base()]) {
										queued[stop.station.base()] = true;
										next.push_back(stop.station);
									}
								}
							}
							if (retain_section) this->ride_sections.push_back(ride);
						}
						if (stop.must_leave) break;
					}
				}
			}
			if (interchange_only) break;
			frontier.clear();
			for (StationID station : next) {
				frontier.emplace_back(station, arrivals[station.base()]);
				queued[station.base()] = false;
			}
			next.clear();
		}
		for (size_t i = 0; i < options.size(); ++i) {
			const auto &by_rides = options[i];
			if (by_rides.empty()) continue;
			const StationID station{static_cast<uint16_t>(i)};
			int64_t fastest = INT64_MAX;
			const Connection *quickest = nullptr;
			for (const auto &[rides, choices] : by_rides) {
				if (choices.timed[0].board && choices.timed[0].arrival < fastest) {
					fastest = choices.timed[0].arrival;
					quickest = &choices.timed[0];
				}
			}
			/* Wait + ride time is measured from now in simulation ticks. Use the
			 * game's clock, never a client's personal display preference. */
			auto arrival_limit = [&](uint rides) {
				if (fastest == INT64_MAX) return fastest;
				return fastest + (rides == 1 ? std::max(15 * ticks_per_minute, fastest / 4) : std::max(8 * ticks_per_minute, fastest / 10));
			};
			const Connection *best = nullptr, *unknown = nullptr;
			for (const auto &[rides, choices] : by_rides) {
				if (choices.untimed.board && !unknown) unknown = &choices.untimed;
				if (!choices.timed[0].board || choices.timed[0].arrival > arrival_limit(rides)) continue;
				best = &choices.timed[0];
				break; // First feasible ride count within the one end-to-end budget.
			}
			if (unknown && (!best || unknown->rides < best->rides)) best = unknown;
			if (!best) continue;
			const int64_t earliest_in_tier = best->arrival;
			const int64_t close_limit = std::min(arrival_limit(best->rides), close_arrival(earliest_in_tier));
			auto &destination = destinations[station];
			destination.close_limit = close_limit;
			if (best->rides > 1) {
				for (auto &[rides, choices] : options[i]) {
					if (rides != best->rides) continue;
					destination.nearby = std::move(choices.nearby_transfers);
					std::erase_if(destination.nearby, [&](const Connection &c) {
						return earliest_in_tier < 0 ? c.arrival >= 0 || c.board->visit != best->board->visit :
								c.arrival < 0 || c.arrival > close_limit;
					});
					break;
				}
			}
			/* Preserve exactly the native transfer tier. Price the remaining alternatives
			 * only when a waiting passenger actually requests this destination. */
			for (auto &[rides, choices] : options[i]) {
				if (rides != best->rides || choices.offers.empty()) continue;
				const auto start = earliest_in_tier;
				auto within_budget = [&](const Connection &c) {
					return c.arrival <= std::min(arrival_limit(rides), start + std::max(minimum_connection, start / 5));
				};
				auto eligible_offer = [&](const Connection &c) {
					if (start >= 0) return c.arrival >= 0 && within_budget(c);
					return c.arrival < 0 && c.board->visit == best->board->visit &&
							(rides > 1 || c.stops == best->stops);
				};
				std::erase_if(choices.offers, [&](const Connection &c) { return !eligible_offer(c); });
				if (!choices.offers.empty()) {
					if (choices.ordinary) {
						const auto &ordinary = (*choices.ordinary)[start >= 0];
						if (ordinary.board && eligible_offer(ordinary)) choices.offers.push_back(ordinary);
					}
					this->fare_offers[{from, station}] = std::move(choices.offers);
				}
			}
			auto &selected = destination.selected = *best;
			if (!quickest) continue;
			/* A full faster route can overturn the preferred transfer count. If a
			 * direct train already won, removing that competitor only widens its lead. */
			if (best->rides > 1) selected.fastest_route = quickest->route;
			destination.fastest_leg = first_leg(*quickest);
			destination.fastest_arrival = fastest;
			for (const auto &[rides, choices] : by_rides) {
				for (const Connection &other : choices.timed) {
					if (other.board && first_leg(other) != destination.fastest_leg && (destination.other_arrival < 0 ||
							other.arrival < destination.other_arrival)) destination.other_arrival = other.arrival;
				}
			}
			selected.alternative_arrival = first_leg(selected) == destination.fastest_leg ? destination.other_arrival : fastest;
		}
	}
	auto result = destinations.find(to);
	if (result == destinations.end()) return nullptr;
	auto &destination = result->second;
	auto &selected = destination.selected;
	if (interchange_only) return &selected;
	/* The frontier reaches many stations nobody in this batch is travelling to.
	 * Resolve and cache mileage only when this particular destination is used. */
	if (!destination.nearby.empty()) {
		const size_t benchmark = selected.fastest_route;
		uint64_t length = this->RouteLength(selected.route);
		for (const Connection &c : destination.nearby) {
			const uint64_t other = this->RouteLength(c.route);
			const bool wins_tie = other == length && (c.arrival < 0 ?
					unknown_rank(c) < unknown_rank(selected) : time_rank(c) < time_rank(selected));
			if (other < length || wins_tie) {
				selected = c;
				length = other;
			}
		}
		selected.fastest_route = benchmark;
		selected.alternative_arrival = first_leg(selected) == destination.fastest_leg ? destination.other_arrival : destination.fastest_arrival;
		destination.nearby.clear();
	}
	const auto key = std::pair{from, to};
	auto pending = this->fare_offers.find(key);
	if (!held && pending != this->fare_offers.end()) {
		const int64_t start = selected.arrival;
		const int64_t minute = std::max<uint16_t>(1, _settings_game.game_time.ticks_per_minute);
		const uint direct_distance = PassengerFareDistance(*Station::Get(from), *Station::Get(to));
		const int32_t reference = UseRouteFares() ? ClampTo<int32_t>((PassengerTariff{}.Price(direct_distance, 0, {}) + 50) / 100) : ReferenceFare(direct_distance);
		auto score = [&](const Connection &c) {
			const int64_t price = c.quoted_fare < 0 ? reference : c.quoted_fare;
			return (start < 0) ? price : c.arrival + price * std::max(10 * minute, start / 4) / std::max(1, reference);
		};
		const Connection native = selected;
		selected.quoted_fare = this->Quote(this->ride_sections[native.route - 1], from, to);
		for (auto &c : pending->second) {
			if (native.rides > 1 && c.arrival <= destination.close_limit && this->RouteLength(c.route) > this->RouteLength(native.route)) continue;
			c.quoted_fare = this->Quote(this->ride_sections[c.route - 1], from, to);
			if (score(c) < score(selected)) selected = c;
		}
		if (selected.route != native.route) {
			this->prepaid_alternatives[key] = native;
			selected.alternative_arrival = native.arrival;
			selected.fastest_route = native.fastest_route;
		}
		this->fare_offers.erase(pending);
	}
	if (held) {
		auto original = this->prepaid_alternatives.find(key);
		if (original != this->prepaid_alternatives.end()) return &original->second;
	}
	return &selected;
}

/** A sale can exhaust a section shared by many itineraries. Recheck just the
 * selected rides before repeating a station-wide search for an alternative. */
bool PassengerServices::SelectedRouteIsFull(StationID from, StationID to, bool interchange, PassengerSeatInventory &seats, bool held) const
{
	if (from == to || !this->Serves(from) || !this->Serves(to)) return false;
	const auto *choice = this->FindConnection(from, to, interchange, held);
	if (!choice) return false;
	auto full = [&](size_t route) {
		while (route != 0) {
			const RideSection &leg = this->ride_sections[route - 1];
			if (leg.end > seats.FirstFullSection(leg.train, leg.start)) return true;
			route = leg.previous;
		}
		return false;
	};
	return full(choice->route) || (choice->fastest_route != choice->route && full(choice->fastest_route));
}

std::vector<PassengerQueryRoute> PassengerServices::QueryRoutes(StationID from, StationID to, int64_t earliest)
{
	std::vector<PassengerQueryRoute> result;
	if (from == to || !Station::IsValidID(from) || !Station::IsValidID(to) || GetCargoTypeByLabel(CT_PASSENGERS) == INVALID_CARGO) return result;
	PassengerServices forecast;
	if (!forecast.Serves(from) || !forecast.Serves(to)) return result;
	PassengerSeatInventory seats;
	std::set<std::vector<std::tuple<VehicleID, uint, uint>>> seen;
	auto collect = [&](PassengerServices &planner) {
		Query query{earliest, {}};
		planner.FindConnection(from, to, false, false, &query);
		for (size_t index : query.routes) {
			const auto &last = planner.ride_sections[index - 1];
			std::vector<const RideSection *> path;
			for (size_t step = index; step != 0; step = path.back()->previous) path.push_back(&planner.ride_sections[step - 1]);
			std::ranges::reverse(path);
			std::vector<std::tuple<VehicleID, uint, uint>> key;
			std::set<StationID> visited{from};
			for (const RideSection *leg : path) {
				if (!visited.insert(leg->end_call->station).second) break;
				key.emplace_back(leg->train, leg->start, leg->end);
			}
			if (key.size() != path.size() || !seen.insert(std::move(key)).second) continue;
			PassengerQueryRoute route{{}, -1};
			if (_settings_game.linkgraph.passenger_through_fares) {
				route.fare = planner.Quote(last, from, to);
				if (route.fare < 0) route.fare = ReferenceFare(PassengerFareDistance(*Station::Get(from), *Station::Get(to)));
			}
			int64_t ready = 0;
			for (const RideSection *leg : path) {
				const auto &board = *leg->board;
				const auto &end = *leg->end_call;
				const Vehicle *v = Vehicle::Get(leg->train);
				const bool timed = ready >= 0 && board.timed;
				const int64_t delay = !board.boarding_now && board.headway ? std::max<int64_t>(0, ready + board.headway / 2 - board.departure) : 0;
				const int64_t arrival = timed && end.timed ? end.arrival + delay : -1;
				route.legs.push_back({leg->train, board.station, end.station, PublicNumber(*v, board),
						timed ? board.departure + delay : -1, arrival, seats.SectionRoom(v->index, leg->start, leg->end), v->type == VehicleType::Road,
						PublicNumber(*v, *(leg->end_call - 1))});
				ready = arrival < 0 ? -1 : arrival + 10 * std::max<uint16_t>(1, _settings_game.game_time.ticks_per_minute) + board.headway / 4;
			}
			result.push_back(std::move(route));
		}
	};
	collect(forecast);
	/* A full feeder can hide another service at an interchange. Search with
	 * known occupied sections too, just as the station does when it sells. */
	if (std::ranges::any_of(result, [](const auto &route) {
		return std::ranges::any_of(route.legs, [](const auto &leg) { return leg.seats == 0; });
	})) {
		PassengerServices available(seats, forecast);
		collect(available);
		while (available.SelectedRouteIsFull(from, to, false, seats)) {
			available.ClearConnections();
			collect(available);
		}
	}
	return result;
}

std::string PassengerServices::PublicNumber(const Vehicle &vehicle, const Call &call)
{
	std::string dispatch;
	if (call.dispatch_slot >= 0) {
		const auto &schedule = vehicle.orders->GetDispatchScheduleByIndex(call.dispatch_schedule);
		const auto route = schedule.GetScheduledDispatch()[call.dispatch_slot].route_id;
		if (route != 0) dispatch = schedule.GetSupplementaryName(DispatchSchedule::SupplementaryNameType::RouteID, route);
	} else {
		dispatch = vehicle.GetActiveDispatchServiceNumber();
	}
	if (const Order *change = call.number_change; change != nullptr) {
		return change->GetServiceNumberMode() == 1 ? std::string(change->GetServiceNumber()) : dispatch;
	} else if (!vehicle.service_number_suppressed) {
		return vehicle.fixed_service_number.empty() ? dispatch : std::string(vehicle.fixed_service_number);
	}
	return {};
}

PassengerJourney PassengerServices::Choose(StationID from, StationID to, std::string *service_number, bool interchange_only, PassengerRouteHint *hint, bool held) const
{
	if (service_number != nullptr) service_number->clear();
	if (from == to || !this->Serves(from) || !this->Serves(to)) return {};
	/* Forecast optimistically for unexamined trains. Each rejected itinerary reveals
	 * a full section for the next search. Only the returned route needs a complete
	 * seat check, which avoids rebuilding seat inventories for the entire network. */
	while (this->inventory && this->SelectedRouteIsFull(from, to, interchange_only, *this->inventory, held)) this->ClearConnections();
	const Connection *connection = this->FindConnection(from, to, interchange_only, held);
	if (!connection) return {};
	PassengerJourney best{connection->train, connection->alight, connection->board->boarding_now, connection->arrival,
			connection->alternative_arrival, connection->board->order, connection->board->visit, connection->quoted_fare, {}};
	const RideSection &last = this->ride_sections[connection->route - 1];
	if (!held && !interchange_only && last.priced) best.quoted_fare = this->Quote(last, from, to, &best.quoted_shares, &best.quoted_distance);
	if (hint && best.arrival >= 0 && connection->rides >= 1 && connection->rides <= 2) {
		hint->destination = to;
		hint->tick = _scaled_tick_counter;
		hint->legs = connection->rides;
		size_t route = connection->route;
		for (uint i = hint->legs; i-- > 0;) {
			const RideSection &leg = this->ride_sections[route - 1];
			const Vehicle *v = Vehicle::Get(leg.train);
			/* Use only the current circuit; the same order can be tomorrow's train. */
			if (leg.start >= v->GetNumOrders() || leg.board->visit != 0) { hint->legs = 0; break; }
			hint->vehicle[i] = v->index;
			hint->schedule[i] = v->orders->index;
			hint->cursor[i] = v->cur_real_order_index;
			hint->board[i] = leg.board->order;
			hint->leave[i] = (v->cur_real_order_index + leg.end) % v->GetNumOrders();
			route = leg.previous;
		}
	}
	const Call *boarding_call = connection->board;
	if (Vehicle::Get(best.train)->type == VehicleType::Road) {
		/* The plan identifies direction and alighting stop. The downstream rail
		 * number is informational until the group actually reaches the station. */
		const RideSection *next = nullptr;
		for (size_t index = connection->route; index != 0;) {
			const auto &ride = this->ride_sections[index - 1];
			if (ride.previous == 0) {
				best.bus_via = (ride.board + 1)->station;
				if (service_number && next && Vehicle::Get(next->train)->type == VehicleType::Train) {
					*service_number = PublicNumber(*Vehicle::Get(next->train), *next->board);
				}
				break;
			}
			next = &ride;
			index = ride.previous;
		}
	} else if (service_number) {
		*service_number = PublicNumber(*Vehicle::Get(best.train), *boarding_call);
	}
	if (boarding_call) {
		best.visit = boarding_call->visit;
		best.schedule = boarding_call->schedule;
	}
	return best;
}

const PassengerServices::Service &PassengerServices::ReplayCache::GetService(const Vehicle &v, bool refresh)
{
	auto [it, fresh] = this->services.try_emplace(v.index);
	if (fresh || refresh) it->second = ForecastService(&v, GetCargoTypeByLabel(CT_PASSENGERS));
	return it->second;
}

/** Correct an inherited fixed number only when the booked departure can still be identified.
 * A dispatch-slot ticket keeps its sold number until the usual validity check. */
std::string_view PassengerServices::ReplayCache::BookedNumber(const PassengerTicket &ticket, StationID station)
{
	const Train *v = Train::GetIfValid(ticket.train);
	if (v == nullptr || v->orders == nullptr || v->orders->index != ticket.schedule || ticket.call >= v->GetNumOrders()) return ticket.number;
	const Order *order = v->GetOrder(ticket.call);
	if (order->GetServiceNumberMode() != 0 || PassengerStop(*order) != station) return ticket.number;
	const auto &calls = this->GetService(*v).calls;
	const uint section = BoardSection(*v, ticket.call, ticket.visit);
	auto it = std::ranges::lower_bound(calls, section, {}, &Call::section);
	if (it == calls.end() || it->section != section || it->station != station || !it->board) return ticket.number;
	if (it->number_change != nullptr) {
		if (it->number_change->GetServiceNumberMode() == 1) return it->number_change->GetServiceNumber();
	} else if (!v->service_number_suppressed && !v->fixed_service_number.empty()) {
		return std::string_view(v->fixed_service_number);
	}
	return ticket.number;
}

/** Replay only the proposed calls. Recompute times, direction, transfer margin,
 * public number and availability from live vehicles before selling any seats. */
PassengerJourney PassengerServices::ReplayCache::Choose(const PassengerRouteHint &hint, StationID from, PassengerSeatInventory &seats, std::string &number, bool station_sale)
{
	PassengerJourney first;
	int64_t ready = 0;
	const StationID origin = from;
	ItineraryFare fare{0};
	const bool route_fares = UseRouteFares();
	bool priced = route_fares;
	for (uint i = 0; !priced && _settings_game.linkgraph.passenger_through_fares && i < hint.legs; ++i) {
		const Vehicle *v = Vehicle::GetIfValid(hint.vehicle[i]);
		if (v && v->orders && (v->passenger_tariff.Active() || v->orders->passenger_tariff.Active() || Company::Get(v->owner)->passenger_tariff.Active())) priced = true;
	}
	if (priced) fare.journey_distance = PassengerFareDistance(*Station::Get(from), *Station::Get(hint.destination));
	for (uint leg = 0; leg < hint.legs; ++leg) {
		const Vehicle *v = Vehicle::GetIfValid(hint.vehicle[leg]);
		if (!v || !v->orders || v->orders->index != hint.schedule[leg] || v->cur_real_order_index != hint.cursor[leg] ||
				(v->type != VehicleType::Train && (v->type != VehicleType::Road || !RoadVehicle::From(v)->IsBus()))) return {};
		/* Several destinations can reuse the same live timetable during one sale.
		 * Rebuild during boarding, where the space on the vehicle can change. */
		const Service &service = this->GetService(*v, !station_sale);
		auto board = std::ranges::find(service.calls, hint.board[leg], &Call::order);
		if (board == service.calls.end() || !board->timed || !board->board || board->station != from ||
				board->visit != 0 || board->departure < ready) return {};
		auto end = board + 1;
		for (; end != service.calls.end(); ++end) {
			if (end->station == from || end->station == origin) return {};
			if (end->order == hint.leave[leg]) break;
			if (end->must_leave) return {};
		}
		if (end == service.calls.end() || !end->timed || !end->alight ||
				(leg + 1 == hint.legs && (!end->deliver || end->station != hint.destination)) ||
				end->section > seats.FirstFullSection(v->index, board->section)) return {};
		if (leg == 0) {
			first.train = v->index;
			first.schedule = board->schedule;
			first.alight = end->station;
			first.board_order = board->order;
			first.boarding_now = board->boarding_now;
			if (v->type == VehicleType::Road) first.bus_via = (board + 1)->station;
		}
		if (v->type == VehicleType::Train && (leg == 0 || first.bus_via != StationID::Invalid())) number = PublicNumber(*v, *board);
		const int64_t wait = board->boarding_now ? 0 : std::max<int64_t>(0, ready + board->headway / 2 - board->departure);
		first.arrival = end->arrival + wait;
		if (route_fares) {
			const auto length = this->distances.Between(*board, *end);
			if (length > UINT32_MAX - first.quoted_distance) return {};
			fare.AddRouteLeg(*v, board->schedule, end->station, first.quoted_distance, static_cast<uint>(length), end->arrival - board->departure, true);
			first.quoted_distance += static_cast<uint>(length);
		} else if (priced) fare.Add(*v, board->schedule, end->station, PassengerFareDistance(*Station::Get(from), *Station::Get(end->station)), end->arrival - board->departure, true);
		ready = first.arrival + 10 * std::max<uint16_t>(1, _settings_game.game_time.ticks_per_minute) + board->headway / 4;
		from = end->station;
	}
	if (priced) first.quoted_fare = fare.Finish(&first.quoted_shares, route_fares);
	return first;
}

/** Validate the exact boarding call and the remaining ride before allowing a ticket to be used. */
bool PassengerBoarding::IsTicketValid(const PassengerTicket &ticket, StationID destination)
{
	const StationID station = this->station;
	const Train *train = Train::GetIfValid(ticket.train);
	if (train == nullptr || !train->IsPrimaryVehicle() || train->vehstatus.Any({VehState::Stopped, VehState::Crashed}) ||
			train->orders == nullptr || train->orders->index != ticket.schedule) return false;

	/* A ticket reserves a call and a seat interval. Updating measured travel or dwell
	 * time elsewhere on the shared list does not cancel that reservation. */

	const Order *board = train->GetOrder(ticket.call);
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	if (this->inventory.Capacity(*train) == 0 || board == nullptr || PassengerStop(*board) != station || board->GetCargoLoadType(cargo) == OrderLoadType::NoLoad) return false;
	const bool current_call = ticket.visit == 0 && train->cur_real_order_index == ticket.call;
	if (current_call && IsPassengerBoardingAt(*train, station)) {
		if (!PassengerLoadingOpen(*train) || train->GetActiveServiceNumber() != ticket.number) return false;
	} else if (board->GetServiceNumberMode() == 1) {
		if (board->GetServiceNumber() != ticket.number) return false;
	} else if (current_call && train->current_order.IsType(OT_GOTO_STATION) && train->current_order.GetDestination() == station &&
			board->GetServiceNumberMode() == 0 && !(train->vehicle_flags.Test(VehicleFlag::ScheduledDispatch) && board->IsScheduledDispatchOrder(true))) {
		/* An inherited number is already known on this approach, including for tickets loaded from older saves. */
		if (train->GetActiveServiceNumber() != ticket.number) return false;
	}

	/* Packets sharing this call and alighting stop use the same order suffix
	 * throughout one sale. Check the current number and train above for each ticket. */
	const uint64_t key = (uint64_t{ticket.train.base()} << 33) | (uint64_t{ticket.call} << 17) |
			(uint64_t{ticket.alight.base()} << 1) | (ticket.alight == destination);
	auto [entry, fresh] = this->valid_rides.try_emplace(key, false);
	if (!fresh) return entry->second;
	for (uint i = 1; i < train->GetNumOrders(); ++i) {
		const VehicleOrderID index = (ticket.call + i) % train->GetNumOrders();
		const Order *o = train->GetOrder(index);
		if (IsPassengerAdministrativeOrder(*o)) continue;
		if ((o->IsType(OT_DECOUPLE) || o->IsType(OT_WAIT_COUPLE)) && IsThroughPassengerExchange(*train, index, cargo)) continue;
		if (!o->IsType(OT_GOTO_STATION) && !o->IsType(OT_GOTO_WAYPOINT) && !o->IsExternalTripOrder()) return false;
		const StationID stop = PassengerStop(*o);
		if (stop == StationID::Invalid()) { if (o->IsExternalTripOrder()) return false; continue; }
		if (stop == station) return false;
		const auto unload = o->GetCargoUnloadType(cargo);
		if (stop == ticket.alight && unload != OrderUnloadType::NoUnload) return entry->second = stop != destination || unload != OrderUnloadType::Transfer;
		if (unload == OrderUnloadType::Transfer || unload == OrderUnloadType::Unload) return false;
	}
	return false;
}

void PassengerBoarding::InvalidateSeatChoices(bool reopened)
{
	this->tickets.clear();
	this->prepaid_tickets.clear();
	this->interchange_tickets.clear();
	/* Selling the last seat on one section cannot create a new connection.
	 * Recheck a cached interchange when it is requested. A cancellation can
	 * open a previously unreachable path, so it also invalidates negative results. */
	if (reopened) {
		this->interchange_by_origin.clear();
		this->onward_hops.clear();
	}
}

const PassengerTicket &PassengerBoarding::TicketFor(StationID stop, bool interchange, bool held)
{
	auto &cache = interchange ? this->interchange_tickets : held ? this->prepaid_tickets : this->tickets;
	auto [it, inserted] = cache.try_emplace(stop);
	if (inserted && IsPassengerTerminal(*Station::Get(stop)) && (!interchange || Station::Get(stop)->external_city == UINT16_MAX)) {
		auto &ticket = it->second;
		auto &routes = Station::Get(this->station)->passenger_routes;
		PassengerJourney choice;
		if (!interchange && !held) {
			auto found = routes.find(stop);
			if (found != routes.end() && _scaled_tick_counter - found->second.tick < 512) {
				choice = this->replays.Choose(found->second, this->station, this->inventory, ticket.number, this->station_sale);
			}
		}
		if (choice.train == VehicleID::Invalid()) {
			if (!this->services) this->services.emplace();
			PassengerServices *planner = &*this->services;
			if (planner->SelectedRouteIsFull(this->station, stop, interchange, this->inventory, held)) {
				if (!this->limited) this->limited.emplace(this->inventory, *this->services);
				planner = &*this->limited;
			}
			PassengerRouteHint hint{};
			choice = planner->Choose(this->station, stop, &ticket.number, interchange, interchange || held ? nullptr : &hint, held);
			if (!interchange && !held) {
				routes.erase(stop);
				if (hint.legs != 0 && routes.size() >= 128) std::erase_if(routes, [](const auto &r) { return _scaled_tick_counter - r.second.tick >= 512; });
				if (hint.legs != 0 && routes.size() < 128) routes.emplace(stop, hint);
			}
			if (planner != &*this->services && choice.train == VehicleID::Invalid()) ticket.rebooking = PassengerRebooking::Full;
		}
		ticket.quoted_fare = choice.quoted_fare;
		ticket.quoted_distance = choice.quoted_distance;
		ticket.quoted_shares = std::move(choice.quoted_shares);
		if (choice.bus_via != StationID::Invalid()) {
			ticket.bus_via = choice.bus_via;
			ticket.alight = choice.alight;
			ticket.rebooking = PassengerRebooking::Bus;
			ticket.reviewed = true;
		} else if (const Train *train = Train::GetIfValid(choice.train); train != nullptr) {
			ticket.train = train->index;
			ticket.schedule = choice.schedule;
			ticket.call = choice.board_order;
			ticket.visit = choice.visit;
			ticket.capacity = this->inventory.Capacity(*train);
			ticket.alight = choice.alight;
			ticket.timetable_duration = train->orders->GetTimetableDurationIncomplete();
			ticket.untimed = choice.arrival < 0;
		}
	}
	return it->second;
}

/** Follow the same origin-specific links used at unloading. On a shortest path, ride as far
 * as possible on the first train. A missing onward link never becomes a ticket to nowhere. */
StationID PassengerBoarding::FindInterchange(StationID origin, StationID destination)
{
	if (!this->services) this->services.emplace();
	if (!this->services->Serves(this->station)) return StationID::Invalid();
	auto [it, inserted] = this->interchange_by_origin.try_emplace(origin);
	auto &alight = it->second;
	if (!inserted) {
		auto found = alight.find(destination);
		if (found == alight.end() || found->second == StationID::Invalid()) return StationID::Invalid();
		const auto &ride = this->TicketFor(found->second, true);
		if (ride.train != VehicleID::Invalid() || ride.rebooking == PassengerRebooking::Bus) return found->second;
		alight.clear();
	}
	{
		const auto cargo = GetCargoTypeByLabel(CT_PASSENGERS);
		std::vector<StationID> queue{this->station};
		alight.emplace(this->station, StationID::Invalid());
		for (size_t i = 0; i < queue.size(); ++i) {
			const auto at = queue[i];
			const auto &flows = Station::Get(at)->goods[cargo].ConstFlows();
			auto flow = flows.find(origin);
			if (flow == flows.end()) continue;
			StationID through = alight.at(at);
			bool checked_train = at == this->station;
			for (const auto &share : *flow) {
				if (share.first > flow->GetUnrestricted()) break;
				const StationID next = share.second;
				if (!Station::IsValidID(next)) continue;
				if (alight.contains(next)) continue;
				if (!checked_train) {
					const auto &ride = this->TicketFor(at, true);
					if (ride.train != VehicleID::Invalid() || ride.rebooking == PassengerRebooking::Bus) through = at;
					checked_train = true;
				}
				alight.emplace(next, through);
				queue.push_back(next);
			}
		}
	}
	auto result = alight.find(destination);
	return result == alight.end() ? StationID::Invalid() : result->second;
}

/** A transferred passenger can outlive the flow calculated for their original
 * station. Use a current unrestricted passenger link towards their destination
 * only if a real service can take them to that interchange. Among shortest
 * paths, prefer one with a bookable first leg. Share the search across this sale. */
StationID PassengerBoarding::FindOnwardHop(StationID destination)
{
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	const GoodsEntry &from = Station::Get(this->station)->goods[cargo];
	const GoodsEntry &to = Station::Get(destination)->goods[cargo];
	const LinkGraph *graph = LinkGraph::GetIfValid(from.link_graph);
	if (graph == nullptr || from.link_graph != to.link_graph || from.node >= graph->Size() || to.node >= graph->Size()) return StationID::Invalid();
	if (!this->onward_hops.empty()) {
		const auto next = this->onward_hops[to.node];
		if (next == StationID::Invalid()) return next;
		const auto &ride = this->TicketFor(next, true);
		if (ride.train != VehicleID::Invalid() || ride.rebooking == PassengerRebooking::Bus) return next;
	}
	{
		this->onward_hops.assign(graph->Size(), StationID::Invalid());
		std::vector<uint> distance(graph->Size(), UINT_MAX);
		std::vector<NodeID> queue{from.node};
		bool can_depart = false;
		distance[from.node] = 0;
		for (size_t i = 0; i < queue.size(); ++i) {
			if (i != 0 && !can_depart) break;
			const NodeID at = queue[i];
			if (i != 0 && Station::Get((*graph)[at].Station())->IsExternalMarket()) continue;
			graph->IterateEdgesFromNode(at, [&](NodeID, NodeID next, LinkGraph::ConstEdge edge) {
				if (edge.Capacity() == 0 || edge.LastUnrestrictedUpdate() == EconTime::INVALID_DATE || distance[next] < distance[at] + 1) return;
				if (distance[next] == UINT_MAX) {
					distance[next] = distance[at] + 1;
					queue.push_back(next);
				}
				if (this->onward_hops[next] != StationID::Invalid()) return;
				if (at == from.node) {
					const StationID stop = (*graph)[next].Station();
					const auto &ride = this->TicketFor(stop, true);
					if (ride.train != VehicleID::Invalid() || ride.rebooking == PassengerRebooking::Bus) {
						this->onward_hops[next] = stop;
						can_depart = true;
					}
				} else {
					this->onward_hops[next] = this->onward_hops[at];
				}
			});
		}
	}
	return this->onward_hops[to.node];
}

/** Directed passenger links from an interchange, shared by all destinations
 * in this sale. The graph is evidence of onward service, not a seat reservation. */
uint PassengerBoarding::OnwardDistance(StationID from, StationID to, uint max_hops)
{
	const auto cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	const auto &a = Station::Get(from)->goods[cargo];
	const auto &b = Station::Get(to)->goods[cargo];
	const auto *graph = LinkGraph::GetIfValid(a.link_graph);
	if (!graph || a.link_graph != b.link_graph || a.node >= graph->Size() || b.node >= graph->Size()) return UINT_MAX;
	auto [it, fresh] = this->onward_distances.try_emplace(from);
	auto &[dist, queue, cursor] = it->second;
	if (fresh) {
		dist.assign(graph->Size(), UINT16_MAX);
		dist[a.node] = 0;
		queue.push_back(a.node);
	}
	/* Once the nearer platform needs N links, exploring more than N links
	 * from the old interchange cannot change the comparison. Later buyers
	 * resume the same breadth-first search only if they need to go farther. */
	while (dist[b.node] == UINT16_MAX && cursor < queue.size() && dist[queue[cursor]] < max_hops) {
		const NodeID at = queue[cursor++];
		if (Station::Get((*graph)[at].Station())->IsExternalMarket()) continue;
		graph->IterateEdgesFromNode(at, [&](NodeID, NodeID next, LinkGraph::ConstEdge edge) {
			if (dist[next] != UINT16_MAX || edge.Capacity() == 0 || edge.LastUnrestrictedUpdate() == EconTime::INVALID_DATE) return;
			dist[next] = dist[at] + 1;
			queue.push_back(next);
		});
	}
	return dist[b.node] == UINT16_MAX || dist[b.node] > max_hops ? UINT_MAX : dist[b.node];
}

PassengerBoarding::LaterStop PassengerBoarding::BetterInterchange(const PassengerTicket &ticket, StationID destination, uint &start)
{
	LaterStop best{ticket.alight, 0, false};
	const Train *train = Train::GetIfValid(ticket.train);
	if (!train || !train->orders || ticket.call >= train->GetNumOrders() || destination == ticket.alight) return best;
	auto [it, fresh] = this->later_stops.try_emplace(std::tuple{ticket.train, ticket.call, ticket.alight});
	auto &stops = it->second;
	if (fresh) {
		const uint orders = train->GetNumOrders();
		const auto cargo = GetCargoTypeByLabel(CT_PASSENGERS);
		std::set<StationID> visited{this->station};
		for (uint offset = 1; offset < orders; ++offset) {
			const auto call = (ticket.call + offset) % orders;
			if (call == 0 && train->IsExecutingSchedule() && train->GetPrimaryOrderList() != train->orders) break;
			const Order &order = *train->GetOrder(call);
			if (IsPassengerAdministrativeOrder(order) || order.IsType(OT_GOTO_WAYPOINT)) continue;
			if ((order.IsType(OT_DECOUPLE) || order.IsType(OT_WAIT_COUPLE)) && IsThroughPassengerExchange(*train, call, cargo)) continue;
			if (!order.IsType(OT_GOTO_STATION) && !order.IsExternalTripOrder()) break;
			const auto stop = PassengerStop(order);
			if (!Station::IsValidID(stop)) { if (order.IsExternalTripOrder()) break; continue; }
			if (!visited.insert(stop).second) break; // Do not sell a trip around the return loop.
			const auto unload = order.GetCargoUnloadType(cargo);
			if (unload != OrderUnloadType::NoUnload && (stop == ticket.alight || !stops.empty())) {
				stops.push_back({stop, offset, unload != OrderUnloadType::Transfer});
			}
			if (unload == OrderUnloadType::Transfer || unload == OrderUnloadType::Unload || order.IsExternalTripOrder()) break;
		}
	}
	if (stops.size() < 2) return best;
	const uint board = BoardSection(*train, ticket.call, ticket.visit);
	start = board + stops.front().offset;
	const uint limit = this->inventory.FirstFullSection(ticket.train, start);
	const auto &dest = *Station::Get(destination);
	uint distance = PassengerFareDistance(*Station::Get(ticket.alight), dest);
	uint hops = UINT_MAX;
	for (auto later = std::next(stops.begin()); later != stops.end() && board + later->offset <= limit; ++later) {
		if (later->station == destination) {
			if (later->deliver) best = *later;
			break;
		}
		const Station &candidate = *Station::Get(later->station);
		const uint remaining = PassengerFareDistance(candidate, dest);
		if (remaining >= distance || candidate.IsExternalMarket()) continue;
		const uint onward = this->OnwardDistance(later->station, destination, hops);
		if (onward >= hops) continue;
		if (best.station == ticket.alight && this->OnwardDistance(ticket.alight, destination, onward) <= onward) continue;
		best = *later;
		hops = onward;
		distance = remaining;
	}
	if (best.station != ticket.alight) best.offset += board;
	return best;
}

const PassengerTicket &PassengerBoarding::FallbackTicket(StationID stop, StationID destination)
{
	const auto &short_leg = this->TicketFor(stop, true);
	uint start;
	const auto best = this->BetterInterchange(short_leg, destination, start);
	if (best.station == short_leg.alight) return short_leg;
	this->extended_ticket = short_leg;
	this->extended_ticket.alight = best.station;
	return this->extended_ticket;
}

const PassengerTicket &PassengerBoarding::NextLeg(const CargoPacket &packet)
{
	const StationID dest = packet.passenger_destination;
	const auto &leg = this->TicketFor(dest, false, packet.through_fare >= 0);
	if (leg.train == VehicleID::Invalid() && leg.rebooking != PassengerRebooking::Full && leg.rebooking != PassengerRebooking::Bus) {
		const StationID change = this->FindInterchange(packet.GetFirstStation(), dest);
		if (change != StationID::Invalid() && change != dest) return this->FallbackTicket(change, dest);
		if (packet.GetFirstStation() != this->station && (packet.ticket.rebooking != PassengerRebooking::None || packet.through_fare >= 0)) {
			const StationID next = this->FindOnwardHop(dest);
			if (next != StationID::Invalid() && next != dest && next != packet.GetFirstStation()) return this->FallbackTicket(next, dest);
		}
	}
	return leg;
}

/** Return the unsold part of a split group to the caller, which owns its station queue. */
CargoPacket *PassengerBoarding::CheckTicket(CargoPacket &packet, bool review_only)
{
	const StationID dest = packet.passenger_destination;
	if (!Station::IsValidID(dest) || dest == this->station) return nullptr;
	if (packet.ticket.train != VehicleID::Invalid()) {
		const Train *train = Train::GetIfValid(packet.ticket.train);
		if (this->station_sale) {
			const auto number = this->replays.BookedNumber(packet.ticket, this->station);
			if (number != packet.ticket.number) packet.ticket.number = number;
		}
		const bool valid_call = this->IsTicketValid(packet.ticket, dest);
		if (valid_call) {
			const uint capacity = this->inventory.Capacity(*train);
			/* Two bookings may now overlap even if the number of seats did not
			 * change. Reconcile them in issue order at the station sale. */
			if (!this->station_sale && packet.ticket.capacity == capacity) return nullptr;
			const uint retained = this->inventory.Retained(packet);
			if (retained == packet.Count()) {
				packet.ticket.capacity = capacity;
				/* Old saves have unreviewed rail tickets. Keep a confirmed connecting
				 * itinerary and its fare. Only extend an incomplete transfer booking. */
				if (this->station_sale && !packet.ticket.reviewed) {
					packet.ticket.reviewed = true;
					if (packet.ticket.alight != dest && packet.ticket.quoted_fare < 0) {
						uint start;
						const auto best = this->BetterInterchange(packet.ticket, dest, start);
						if (best.station != packet.ticket.alight) {
							if (!this->services) this->services.emplace();
							if (!this->services->HasItinerary(this->station, dest)) {
								auto *rest = this->inventory.ExtendReservation(packet, best.station, start, best.offset);
								this->InvalidateSeatChoices();
								return rest;
							}
						}
					}
				}
				return nullptr;
			}
			if (retained > 0) {
				CargoPacket *rest = packet.Split(packet.Count() - retained);
				if (rest != nullptr) {
					rest->SetPassengerTicket({});
					rest->ticket.rebooking = PassengerRebooking::ServiceChanged;
					packet.ticket.capacity = capacity;
					return rest;
				}
			}
		}
		const bool closed = train != nullptr && packet.ticket.visit == 0 && train->cur_real_order_index == packet.ticket.call &&
				train->last_station_visited == this->station && train->current_order.IsAnyLoadingType() &&
				!PassengerLoadingOpen(*train);
		/* Excess tickets were allocated zero places by Retained(). Dropping one
		 * must not replay the train's complete waiting list for every late buyer. */
		if (!valid_call && this->inventory.Cancel(packet, this->station_sale)) {
			if (this->limited) this->limited->ClearConnections();
			this->InvalidateSeatChoices(true);
		}
		PassengerTicket pending;
		pending.number = packet.ticket.number;
		pending.rebooking = closed ? PassengerRebooking::Departed : PassengerRebooking::ServiceChanged;
		packet.SetPassengerTicket(std::move(pending));
	}
	if (review_only) return nullptr;
	const PassengerTicket *replacement = &this->NextLeg(packet);
	if (replacement->rebooking == PassengerRebooking::Bus) {
		packet.SetPassengerTicket(*replacement);
		return nullptr;
	}
	if (replacement->train == VehicleID::Invalid()) {
		if (packet.ticket.rebooking == PassengerRebooking::Bus) {
			PassengerTicket pending;
			if (packet.GetFirstStation() != this->station) pending.rebooking = PassengerRebooking::Connection;
			packet.SetPassengerTicket(std::move(pending));
		}
		packet.ticket.reviewed = true;
		if (replacement->rebooking == PassengerRebooking::Full && packet.ticket.rebooking == PassengerRebooking::None) packet.ticket.rebooking = PassengerRebooking::Full;
		if (replacement->rebooking != PassengerRebooking::Full && packet.ticket.rebooking == PassengerRebooking::Full) packet.ticket.rebooking = PassengerRebooking::None;
		return nullptr;
	}
	const auto &t = *replacement;
	const uint room = this->inventory.Room(t.train, t.call, t.visit, t.alight);
	if (room == 0) return nullptr;
	CargoPacket *rest = nullptr;
	if (packet.Count() > room) {
		rest = packet.Split(packet.Count() - room);
		if (rest == nullptr) {
			packet.ticket.rebooking = PassengerRebooking::Full;
			return nullptr;
		}
		rest->ticket.rebooking = PassengerRebooking::Full;
	}
	this->inventory.Use(t.train, t.call, t.visit, t.alight, packet.Count());
	packet.SetPassengerTicket(t);
	packet.ticket.reviewed = true;
	packet.LockThroughFare(*Station::Get(this->station));
	packet.ticket.issued = this->inventory.NextIssue(t.train);
	if (room == packet.Count()) this->InvalidateSeatChoices();
	return rest;
}

static constexpr uint PASSENGER_SALE_INTERVAL = 256;

/** Coalesce transfers and released seats into the platform's existing saved
 * due time. Older platforms keep their place even under constant turnover. */
void RequestOnwardPassengerSale(Station &station)
{
	const uint64_t now = _scaled_tick_counter;
	/* Gradual unloading can produce another batch for every coach. Allow one
	 * loading round for the previous sale to be used before bringing it forward. */
	if (station.next_passenger_sale > now + PASSENGER_SALE_INTERVAL - 32) return;
	const uint64_t due = now > PASSENGER_SALE_INTERVAL ? now - PASSENGER_SALE_INTERVAL : 1;
	if (station.next_passenger_sale == 0 || station.next_passenger_sale > due) station.next_passenger_sale = due;
}

/** Serve the oldest due platform first. This is a simulation work budget, never
 * a wall-clock cutoff. Saving each platform's due time also preserves its turn
 * across a reload or a client joining a busy game. */
Station *NextPassengerSale()
{
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	if (!_settings_game.linkgraph.passenger_ticketing || cargo == INVALID_CARGO ||
			_settings_game.linkgraph.GetDistributionType(cargo) == DistributionType::Manual) return nullptr;
	/* A network larger than the background refresh interval can keep the
	 * scheduler continuously backlogged. Spread background sales over more ticks
	 * on large maps. Arrival sales still run immediately before boarding. */
	constexpr uint refresh_interval = PASSENGER_SALE_INTERVAL;
	const uint stations = Station::GetNumItems();
	const uint interval = stations > 4 * refresh_interval ? 4 : stations > refresh_interval ? 2 : 1;
	if (_tick_counter % interval != 0) return nullptr;
	const uint64_t now = _scaled_tick_counter;
	Station *oldest = nullptr;
	for (Station *st : Station::Iterate()) {
		if (!st->facilities.Any({StationFacility::Train, StationFacility::BusStop}) || st->goods[cargo].ConstCargoList().AvailableCount() == 0) continue;
		if (st->next_passenger_sale == 0) st->next_passenger_sale = now + ((0U - now - st->index.base()) & (refresh_interval - 1));
		if (st->next_passenger_sale <= now && (!oldest || st->next_passenger_sale < oldest->next_passenger_sale)) oldest = st;
	}
	if (oldest) oldest->next_passenger_sale = now + refresh_interval;
	return oldest;
}

void SellWaitingPassengerTickets(Station &station, bool background)
{
	if (!_settings_game.linkgraph.passenger_ticketing) return;
	if (!background) {
		station.passenger_routes.clear(); // A train is actually offering a new departure here.
		station.next_passenger_sale = _scaled_tick_counter + PASSENGER_SALE_INTERVAL;
	}
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	if (cargo == INVALID_CARGO || station.goods[cargo].data == nullptr ||
			_settings_game.linkgraph.GetDistributionType(cargo) == DistributionType::Manual) return;
	if (!IsPassengerTerminal(station)) return;
	PassengerBoarding sale(station.index, true);
	station.goods[cargo].data->cargo.SellPassengerSeats(sale);
}

/** An edited departure number applies to tickets for this visit, including tickets sold before the edit. */
void RefreshEditedDepartureTickets(OrderListID schedule, VehicleOrderID call, const Order &order)
{
	if (!_settings_game.linkgraph.passenger_ticketing) return;
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	Station *station = Station::GetIfValid(PassengerStop(order));
	if (station == nullptr || cargo == INVALID_CARGO || _settings_game.linkgraph.GetDistributionType(cargo) == DistributionType::Manual) return;
	bool resell = false;
	for (const auto &[via, packets] : *station->goods[cargo].ConstCargoList().Packets()) {
		for (CargoPacket *packet : packets) {
			auto &ticket = packet->ticket;
			if (ticket.train == VehicleID::Invalid() || ticket.schedule != schedule || ticket.call != call) continue;
			if (order.GetServiceNumberMode() == 1) {
				ticket.number = order.GetServiceNumber();
			} else {
				packet->SetPassengerTicket({});
				resell = true;
			}
		}
	}
	if (resell) {
		PassengerBoarding sale(station->index, true);
		station->goods[cargo].data->cargo.SellPassengerSeats(sale);
	}
	SetWindowDirty(WindowClass::StationView, station->index);
}

static void CancelPassengerCall(const Vehicle &train, StationID stop, VehicleOrderID call, PassengerRebooking reason)
{
	const Station *station = Station::GetIfValid(stop);
	const CargoType cargo = GetCargoTypeByLabel(CT_PASSENGERS);
	if (station == nullptr || cargo == INVALID_CARGO || station->goods[cargo].data == nullptr) return;
	bool changed = false;
	uint notified = 0;
	for (const auto &[via, packets] : *station->goods[cargo].ConstCargoList().Packets()) {
		for (CargoPacket *packet : packets) {
			if (packet->ticket.train != train.index || packet->ticket.call != call) continue;
			if (packet->ticket.visit > 0) {
				--packet->ticket.visit;
			} else {
				/* Notify just the boarding stops on this freed section, once per call. */
				if (train.orders && packet->ticket.schedule == train.orders->index && call < train.GetNumOrders() && train.cur_real_order_index < train.GetNumOrders()) {
					const uint start = BoardSection(train, call, 0);
					const uint length = SeatSections(train, start, packet->ticket.alight, false).second - start;
					for (; notified < length; ++notified) {
						const Order *order = train.GetOrder((call + notified) % train.GetNumOrders());
						Station *affected = Station::GetIfValid(PassengerStop(*order));
						if (affected && order->GetCargoLoadType(cargo) != OrderLoadType::NoLoad) RequestOnwardPassengerSale(*affected);
					}
				}
				PassengerTicket pending;
				pending.number = packet->ticket.number;
				pending.rebooking = reason;
				packet->SetPassengerTicket(std::move(pending));
			}
			changed = true;
		}
	}
	if (changed) SetWindowDirty(WindowClass::StationView, stop);
}

/** Transfer only the tickets of the service actually taken over after a successful
 * shunt. Two trains on the same shared list still have independent seats. */
void FollowPassengerSchedule(VehicleID previous, const Train *successor)
{
	if (!_settings_game.linkgraph.passenger_ticketing) return;
	for (const CargoPacket *p : CargoPacket::TicketsForTrain(previous)) {
		PassengerTicket ticket = p->ticket;
		if (successor && successor->orders && successor->orders->index == ticket.schedule) {
			ticket.train = successor->index;
			ticket.capacity = 0; // Recheck actual seats before boarding the changed consist.
		} else {
			ticket.train = VehicleID::Invalid();
			ticket.rebooking = PassengerRebooking::ServiceChanged;
		}
		CargoPacket::Get(p->index)->SetPassengerTicket(std::move(ticket));
	}
}

void ReleaseMissedPassengerTickets(const Vehicle &train, StationID station)
{
	if (!_settings_game.linkgraph.passenger_ticketing) return;
	CancelPassengerCall(train, station, train.cur_real_order_index, PassengerRebooking::Departed);
}

void CancelSkippedPassengerCalls(const Vehicle &train, VehicleOrderID next)
{
	if (train.type != VehicleType::Train || !_settings_game.linkgraph.passenger_ticketing || train.cur_real_order_index >= train.GetNumOrders()) return;
	for (VehicleOrderID i = train.cur_real_order_index; i != next; i = (i + 1) % train.GetNumOrders()) {
		const Order *order = train.GetOrder(i);
		if (PassengerStop(*order) != StationID::Invalid()) CancelPassengerCall(train, PassengerStop(*order), i, PassengerRebooking::ServiceChanged);
	}
}

bool CanBoardPassengerBus(const Vehicle &vehicle, const PassengerTicket &plan, CargoType cargo, bool final_stop)
{
	if (vehicle.type != VehicleType::Road || !RoadVehicle::From(&vehicle)->IsBus() ||
			vehicle.cur_real_order_index >= vehicle.GetNumOrders()) return false;
	const Order *board = vehicle.GetOrder(vehicle.cur_real_order_index);
	const StationID origin = PassengerStop(*board);
	if (origin == StationID::Invalid() || !IsPassengerBoardingAt(vehicle, origin) ||
			board->GetCargoLoadType(cargo) == OrderLoadType::NoLoad) return false;
	bool first = true;
	for (uint i = 1; i < vehicle.GetNumOrders(); ++i) {
		const Order *order = vehicle.GetOrder((vehicle.cur_real_order_index + i) % vehicle.GetNumOrders());
		if (IsPassengerAdministrativeOrder(*order) || order->IsType(OT_GOTO_WAYPOINT)) continue;
		if (!order->IsType(OT_GOTO_STATION) && !order->IsExternalTripOrder()) break;
		const auto stop = PassengerStop(*order);
		if (stop == StationID::Invalid()) { if (order->IsExternalTripOrder()) break; continue; }
		if (stop == origin || (first && stop != plan.bus_via)) break;
		const auto unload = order->GetCargoUnloadType(cargo);
		if (stop == plan.alight) return unload != OrderUnloadType::NoUnload && (!final_stop || unload != OrderUnloadType::Transfer);
		if (unload == OrderUnloadType::Transfer || unload == OrderUnloadType::Unload) break;
		first = false;
	}
	return false;
}

bool CanContinuePassengerLeg(const Vehicle &v, StationID alight, CargoType cargo)
{
	if (v.cur_real_order_index >= v.GetNumOrders()) return false;
	for (uint i = 1; i < v.GetNumOrders(); ++i) {
		const VehicleOrderID index = (v.cur_real_order_index + i) % v.GetNumOrders();
		const Order *order = v.GetOrder(index);
		if (order && IsPassengerAdministrativeOrder(*order)) continue;
		if (order && (order->IsType(OT_DECOUPLE) || order->IsType(OT_WAIT_COUPLE)) && IsThroughPassengerExchange(v, index, cargo)) continue;
		if (!order || (!order->IsType(OT_GOTO_STATION) && !order->IsType(OT_GOTO_WAYPOINT) && !order->IsExternalTripOrder())) break;
		const auto stop = PassengerStop(*order);
		if (stop == StationID::Invalid()) { if (order->IsExternalTripOrder()) break; continue; }
		auto unload = order->GetCargoUnloadType(cargo);
		if (stop == alight && unload != OrderUnloadType::NoUnload) return true;
		if (unload == OrderUnloadType::Transfer || unload == OrderUnloadType::Unload) break;
	}
	return false;
}
