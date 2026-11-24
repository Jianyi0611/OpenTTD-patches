/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file passenger_journey.h Forecast passenger train calls in simulation time. */
#ifndef PASSENGER_JOURNEY_H
#define PASSENGER_JOURNEY_H

#include "cargopacket.h"
#include "vehicle_type.h"
#include "order_type.h"
#include "station_type.h"
#include "cargo_type.h"
#include "3rdparty/robin_hood/robin_hood.h"
#include <vector>
#include <optional>
#include <map>
#include <memory>

struct PassengerJourney {
	VehicleID train = VehicleID::Invalid();
	StationID alight = StationID::Invalid();
	bool boarding_now = false;
	int64_t arrival = 0;
	int64_t alternative_arrival = -1;
	VehicleOrderID board_order = INVALID_VEH_ORDER_ID;
	uint8_t visit = 0;
	int32_t quoted_fare = -1;
	std::vector<uint32_t> quoted_shares;

	StationID bus_via = StationID::Invalid();
	OrderListID schedule = OrderListID::Invalid(); ///< Plan at the booked departure.
	uint32_t quoted_distance = 0;
};

/** Available passenger places on each successive order-to-order section of a train. */
class PassengerSeatInventory {
	struct TrainSeats {
		std::vector<uint32_t> remaining;
		robin_hood::unordered_flat_map<uint32_t, uint> retained; ///< Lookup only. Ticket priority is determined by issue order.
		uint64_t last_issue = 0;
		bool has_shortfall = false;
	};
	std::map<VehicleID, TrainSeats> trains;
	std::map<VehicleID, uint> capacities;
	TrainSeats &Get(VehicleID train);
public:
	uint Capacity(const Vehicle &train);
	uint Room(VehicleID train, VehicleOrderID call, uint8_t visit, StationID alight);
	uint FirstFullSection(VehicleID train, uint start);
	uint SectionRoom(VehicleID train, uint start, uint end);
	uint FirstKnownFullSection(VehicleID train, uint start) const;
	void Use(VehicleID train, VehicleOrderID call, uint8_t visit, StationID alight, uint count);
	uint64_t NextIssue(VehicleID train) { return ++this->Get(train).last_issue; }
	uint Retained(const CargoPacket &packet);
	bool Cancel(const CargoPacket &packet, bool station_sale);
	CargoPacket *ExtendReservation(CargoPacket &packet, StationID stop, uint start, uint end);
};

struct PassengerRouteHint;

struct PassengerQueryLeg {
	VehicleID vehicle;
	StationID from, to;
	std::string number;
	int64_t departure, arrival;
	uint seats;
	bool bus;
	std::string last_number; ///< Number leaving the final intermediate stop.
};

struct PassengerQueryRoute {
	std::vector<PassengerQueryLeg> legs;
	int32_t fare;
};

class PassengerServices {
	struct Call {
		StationID station;
		int64_t arrival, departure;
		bool board, alight, must_leave, boarding_now, deliver;
		VehicleOrderID order;
		int dispatch_schedule, dispatch_slot;
		bool timed;
		uint8_t visit;
		uint32_t headway; ///< Observed bus interval in the absence of a fixed departure.
		uint section; ///< Order offset in this train's forecast.
		OrderListID schedule; ///< Plan this departure was sold against.
		const Order *number_change; ///< Last explicit number change up to this particular departure.
	};
	struct Service {
		VehicleID train;
		std::vector<Call> calls;
		bool priced = false;
	};
	static std::string PublicNumber(const Vehicle &vehicle, const Call &call);
	static Service ForecastService(const Vehicle *vehicle, CargoType cargo);
	PassengerSeatInventory *inventory;
	struct BoardingCall {
		size_t service, call;
		mutable size_t next_visit = SIZE_MAX;
		size_t NextVisit(const std::vector<Call> &calls) const;
	};
	struct RouteDistances {
		robin_hood::unordered_flat_map<OrderListID, std::vector<uint64_t>> plans;
		uint64_t Between(const Call &board, const Call &end);
	};
	struct Timetable {
		RouteDistances distances;
		std::vector<Service> services;
		std::vector<std::vector<OrderListID>> lines;
		std::vector<std::vector<BoardingCall>> boardings;
		std::vector<bool> expanded;
		std::vector<bool> indexed_stations;
		explicit Timetable();
		const std::vector<BoardingCall> &At(StationID station);
	};
	std::shared_ptr<Timetable> timetable;
	struct RideSection {
		VehicleID train;
		uint start, end;
		size_t previous;
		const Call *board;
		const Call *end_call;
		bool priced; ///< Any leg of this itinerary has a custom tariff.
		uint64_t total_distance = UINT64_MAX; ///< Calculated only for competing routes with similar arrival times.
	};
	int32_t Quote(const RideSection &last, StationID origin, StationID destination, std::vector<uint32_t> *shares = nullptr, uint32_t *distance = nullptr) const;
	mutable std::vector<RideSection> ride_sections;
	uint64_t RouteLength(size_t route) const;
	struct Connection {
		VehicleID train = VehicleID::Invalid();
		StationID alight = StationID::Invalid();
		int64_t arrival = -1;
		const Call *board = nullptr;
		uint rides = 0;
		uint stops = 0;
		size_t route = 0;
		int64_t alternative_arrival = -1;
		int32_t quoted_fare = -1;
		size_t fastest_route = 0; ///< The arrival used to set this destination's time budget must also be feasible.
	};
	struct Destination {
		Connection selected;
		std::vector<Connection> nearby;
		int64_t close_limit = -1;
		std::pair<VehicleID, StationID> fastest_leg;
		int64_t fastest_arrival = -1, other_arrival = -1;
	};
	/** A station sale shares the longer itineraries across all waiting destinations. */
	mutable std::map<std::pair<StationID, bool>, std::map<StationID, Destination>> connections;
	mutable std::map<std::pair<StationID, StationID>, Connection> prepaid_alternatives;
	mutable std::map<std::pair<StationID, StationID>, std::vector<Connection>> fare_offers;
	enum class Demand : uint8_t { None, Prepaid, NeedsQuote };
	mutable std::map<StationID, std::vector<Demand>> waiting_destinations;
	struct Query {
		int64_t earliest;
		std::vector<size_t> routes;
	};
	const Connection *FindConnection(StationID from, StationID to, bool interchange_only, bool held = false, Query *query = nullptr) const;
public:
	static std::vector<PassengerQueryRoute> QueryRoutes(StationID from, StationID to, int64_t earliest);
	explicit PassengerServices(PassengerSeatInventory *inventory = nullptr);
	PassengerServices(PassengerSeatInventory &inventory, const PassengerServices &forecast) : inventory(&inventory), timetable(forecast.timetable) {}
	static std::optional<int64_t> DepartureDelay(const Train *train, StationID station, VehicleOrderID order, uint8_t visit);
	bool SelectedRouteIsFull(StationID from, StationID to, bool interchange, PassengerSeatInventory &seats, bool held = false) const;
	void ClearConnections() const { this->connections.clear(); this->prepaid_alternatives.clear(); this->fare_offers.clear(); this->ride_sections.clear(); }

	bool Serves(StationID station) const { return station.base() < this->timetable->lines.size() && !this->timetable->lines[station.base()].empty(); }
	bool HasItinerary(StationID from, StationID to) const { return this->Serves(to) && this->FindConnection(from, to, false, true) != nullptr; }
	PassengerJourney Choose(StationID from, StationID to, std::string *service_number = nullptr, bool interchange_only = false, PassengerRouteHint *hint = nullptr, bool held = false) const;
	class ReplayCache {
		RouteDistances distances;
		std::map<VehicleID, Service> services;
		const Service &GetService(const Vehicle &v, bool refresh = false);
	public:
		std::string_view BookedNumber(const PassengerTicket &ticket, StationID station);
		PassengerJourney Choose(const PassengerRouteHint &hint, StationID from, PassengerSeatInventory &seats, std::string &number, bool station_sale);
	};
};

/** A private, lazy snapshot for one station loading pass, shared by all its coaches. */
struct PassengerBoarding {
	StationID station;
	PassengerSeatInventory inventory;
	PassengerServices::ReplayCache replays;
	std::optional<PassengerServices> services;
	std::optional<PassengerServices> limited;
	std::map<StationID, PassengerTicket> tickets;
	std::map<StationID, PassengerTicket> interchange_tickets;
	std::map<StationID, PassengerTicket> prepaid_tickets;
	/** For each passenger origin, the first rail leg on an unrestricted flow path to each destination. */
	std::map<StationID, std::map<StationID, StationID>> interchange_by_origin;
	std::vector<StationID> onward_hops;
	struct LaterStop { StationID station; uint offset; bool deliver; };
	std::map<std::tuple<VehicleID, VehicleOrderID, StationID>, std::vector<LaterStop>> later_stops;
	struct OnwardSearch {
		std::vector<uint16_t> distance;
		std::vector<uint16_t> queue;
		size_t cursor = 0;
	};
	std::map<StationID, OnwardSearch> onward_distances;
	uint OnwardDistance(StationID from, StationID to, uint max_hops);
	LaterStop BetterInterchange(const PassengerTicket &ticket, StationID destination, uint &start);
	PassengerTicket extended_ticket;
	const PassengerTicket &FallbackTicket(StationID stop, StationID destination);
	robin_hood::unordered_flat_map<uint64_t, bool> valid_rides;
	bool IsTicketValid(const PassengerTicket &ticket, StationID destination);
	void InvalidateSeatChoices(bool reopened = false);
	const PassengerTicket &TicketFor(StationID stop, bool interchange, bool held = false);
	const PassengerTicket &NextLeg(const CargoPacket &packet);
	StationID FindInterchange(StationID origin, StationID destination);
	StationID FindOnwardHop(StationID destination);
	bool station_sale = false;
	explicit PassengerBoarding(StationID station, bool station_sale = false) : station(station), station_sale(station_sale) {}
	CargoPacket *CheckTicket(CargoPacket &packet, bool review_only = false);
};

void SellWaitingPassengerTickets(Station &station, bool background = true);
Station *NextPassengerSale();
void RefreshEditedDepartureTickets(OrderListID schedule, VehicleOrderID call, const Order &order);
void FollowPassengerSchedule(VehicleID previous, const Train *successor);
bool IsPassengerTerminal(const Station &station);
uint PassengerFareDistance(const Station &from, const Station &to);
std::optional<uint32_t> PassengerBookedDistance(const Vehicle &v, VehicleOrderID call, StationID from, StationID to);
std::optional<Money> EstimatePassengerLegFare(const Vehicle &train, VehicleOrderID call, const Station &from, const Station &to);
bool IsPassengerBoardingAt(const Vehicle &vehicle, StationID station);
bool PassengerLoadingOpen(const Vehicle &vehicle);
void RequestOnwardPassengerSale(Station &station);
void ReleaseMissedPassengerTickets(const Vehicle &train, StationID station);
void CancelSkippedPassengerCalls(const Vehicle &train, VehicleOrderID next);
bool CanBoardPassengerBus(const Vehicle &vehicle, const PassengerTicket &plan, CargoType cargo, bool final_stop);
bool CanContinuePassengerLeg(const Vehicle &vehicle, StationID alight, CargoType cargo);
#endif
