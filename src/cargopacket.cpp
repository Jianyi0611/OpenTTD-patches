/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file cargopacket.cpp Implementation of the cargo packets. */

#include "stdafx.h"
#include "command_type.h"
#include "debug.h"
#include "date_func.h"
#include "station_base.h"
#include "core/pool_func.hpp"
#include "core/random_func.hpp"
#include "core/hash_func.hpp"
#include "economy_base.h"
#include "economy_func.h"
#include "cargoaction.h"
#include "passenger_journey.h"
#include "vehicle_base.h"
#include "cargotype.h"
#include "order_type.h"
#include "company_func.h"
#include "company_base.h"
#include "external_city.h"
#include "core/backup_type.hpp"
#include "string_func.h"
#include "strings_func.h"
#include "settings_type.h"
#include "3rdparty/cpp-btree/btree_map.h"

#include "table/strings.h"

#include <vector>
#include <set>
#include "3rdparty/cpp-btree/btree_set.h"
#include <map>

#include "safeguards.h"

/* Initialize the cargopacket-pool */
CargoPacketPool _cargopacket_pool("CargoPacket");
INSTANTIATE_POOL_METHODS(CargoPacket)

btree::btree_map<uint64_t, Money> _cargo_packet_deferred_payments;

/** Derived index. The saved packets are authoritative, including after joining a game. */
static std::map<VehicleID, btree::btree_set<CargoPacketID>> _waiting_tickets;
static bool _waiting_tickets_ready = false;

std::vector<const CargoPacket *> CargoPacket::TicketsForTrain(VehicleID train)
{
	if (!_waiting_tickets_ready) {
		_waiting_tickets.clear();
		for (const CargoPacket *p : CargoPacket::Iterate()) {
			if (p->ticket.train != VehicleID::Invalid()) _waiting_tickets[p->ticket.train].insert(p->index);
		}
		_waiting_tickets_ready = true;
	}
	std::vector<const CargoPacket *> result;
	auto it = _waiting_tickets.find(train);
	if (it != _waiting_tickets.end()) {
		result.reserve(it->second.size());
		for (CargoPacketID id : it->second) result.push_back(CargoPacket::Get(id));
	}
	return result;
}

void CargoPacket::SetPassengerTicket(PassengerTicket replacement)
{
	if (_waiting_tickets_ready && this->ticket.train != replacement.train) {
		if (this->ticket.train != VehicleID::Invalid()) {
			auto it = _waiting_tickets.find(this->ticket.train);
			if (it != _waiting_tickets.end()) {
				it->second.erase(this->index);
				if (it->second.empty()) _waiting_tickets.erase(it);
			}
		}
		if (replacement.train != VehicleID::Invalid()) _waiting_tickets[replacement.train].insert(this->index);
	}
	this->ticket = std::move(replacement);
}

void ClearCargoPacketDeferredPayments() {
	_waiting_tickets_ready = false;
	_waiting_tickets.clear();
	_cargo_packet_deferred_payments.clear();
}

/** Keys below this value are planned alighting stations; larger keys identify completed carrier shares. */
static constexpr uint32_t FARE_OPERATOR = 1U << 16;

void ChangeOwnershipOfCargoPacketDeferredPayments(Owner old_owner, Owner new_owner)
{
	for (CargoPacket *packet : CargoPacket::Iterate()) {
		auto &shares = packet->fare_shares;
		for (size_t i = 0; i + 1 < shares.size(); i += 2) {
			if (shares[i] < FARE_OPERATOR || (shares[i] - FARE_OPERATOR) / 4 != old_owner.base()) continue;
			shares[i] = FARE_OPERATOR + (new_owner == INVALID_OWNER ? MAX_COMPANIES : new_owner.base()) * 4 + (shares[i] % 4);
		}
		auto &miles = packet->fare_mileage;
		for (uint type = 0; type < 4; ++type) {
			const uint old_slot = old_owner.base() * 4 + type;
			if (old_slot >= miles.size() || miles[old_slot] == 0) continue;
			const uint32_t distance = std::exchange(miles[old_slot], 0);
			if (new_owner != INVALID_OWNER) {
				const uint slot = new_owner.base() * 4 + type;
				if (slot >= miles.size()) miles.resize(slot + 1);
				miles[slot] = ClampTo<uint32_t>(uint64_t(miles[slot]) + distance);
			}
		}
		while (!miles.empty() && miles.back() == 0) miles.pop_back();
	}
	std::vector<std::pair<uint64_t, Money>> to_merge;
	auto iter = _cargo_packet_deferred_payments.begin();
	while (iter != _cargo_packet_deferred_payments.end()) {
		uint64_t k = iter->first;
		if ((CompanyID) GB(k, 24, 8) == old_owner) {
			if (new_owner != INVALID_OWNER) {
				SB(k, 24, 8, new_owner.base());
				to_merge.push_back({ k, iter->second });
			}
			iter = _cargo_packet_deferred_payments.erase(iter);
		} else {
			++iter;
		}
	}
	for (auto &m : to_merge) {
		_cargo_packet_deferred_payments[m.first] += m.second;
	}
}

inline uint64_t CargoPacketDeferredPaymentKey(CargoPacketID id, CompanyID cid, VehicleType type)
{
	return (((uint64_t) id.base()) << 32) | (cid.base() << 24) | (to_underlying(type) << 22);
}

template <typename F>
inline void IterateCargoPacketDeferredPayments(CargoPacketID index, bool erase_range, F functor)
{
	auto start_iter = _cargo_packet_deferred_payments.lower_bound(CargoPacketDeferredPaymentKey(index, (CompanyID) 0, (VehicleType) 0));
	auto iter = start_iter;
	for (; iter != _cargo_packet_deferred_payments.end() && iter->first >> 32 == index; ++iter) {
		functor(iter->second, (CompanyID) GB(iter->first, 24, 8), (VehicleType) GB(iter->first, 22, 2));
	}
	if (erase_range) {
		_cargo_packet_deferred_payments.erase(start_iter, iter);
	}
}

std::string DumpCargoPacketDeferredPaymentStats()
{
	Money payments[256][4] = {};
	for (auto &it : _cargo_packet_deferred_payments) {
		payments[GB(it.first, 24, 8)][GB(it.first, 22, 2)] += it.second;
	}

	format_buffer buffer;
	for (uint i = 0; i < 256; i++) {
		for (uint j = 0; j < 4; j++) {
			if (payments[i][j] != 0) {
				AppendStringInPlace(buffer, STR_COMPANY_NAME, i);
				buffer.append(" (");
				AppendStringInPlace(buffer, STR_REPLACE_VEHICLE_TRAIN + j);
				buffer.append("): ");
				AppendStringInPlace(buffer, STR_JUST_CURRENCY_LONG, payments[i][j]);
				buffer.push_back('\n');
			}
		}
	}
	buffer.format("Deferred payment count: {}\n", _cargo_packet_deferred_payments.size());
	buffer.format("Total cargo packets: {}\n", CargoPacket::GetNumItems());
	return buffer.to_string();
}

/**
 * Create a new packet for savegame loading.
 * @param index Index into the cargo packet pool.
 */
CargoPacket::CargoPacket(CargoPacketID index) : PoolItemBase(index)
{
}

/**
 * Creates a new cargo packet.
 *
 * @param index Index into the cargo packet pool.
 * @param first_station Source station of the packet.
 * @param count         Number of cargo entities to put in this packet.
 * @param source        Source of the packet (for subsidies).
 * @pre count != 0
 */
CargoPacket::CargoPacket(CargoPacketID index, StationID first_station,uint16_t count, Source source) :
		PoolItemBase(index),
		count(count),
		source(source),
		first_station(first_station)
{
	dbg_assert(count != 0);
}

/**
 * Create a new cargo packet. Used for older savegames to load in their partial data.
 *
 * @param index Index into the cargo packet pool.
 * @param count              Number of cargo entities to put in this packet.
 * @param periods_in_transit Number of cargo aging periods the cargo has been in transit.
 * @param first_station      Station the cargo was initially loaded.
 * @param source_xy          Station location the cargo was initially loaded.
 * @param feeder_share       Feeder share the packet has already accumulated.
 */
CargoPacket::CargoPacket(CargoPacketID index, uint16_t count, uint16_t periods_in_transit, StationID first_station, TileIndex source_xy, Money feeder_share) :
		PoolItemBase(index),
		count(count),
		periods_in_transit(periods_in_transit),
		feeder_share(feeder_share),
		source_xy(source_xy),
		first_station(first_station)
{
	assert(count != 0);
}

/**
 * Creates a new cargo packet. Used when loading or splitting packets.
 *
 * @param index Index into the cargo packet pool.
 * @param count         Number of cargo entities to put in this packet.
 * @param feeder_share  Feeder share the packet has already accumulated.
 * @param original      The original packet we are splitting.
 */
CargoPacket::CargoPacket(CargoPacketID index, uint16_t count, Money feeder_share, const CargoPacket &original) :
		PoolItemBase(index),
		count(count),
		periods_in_transit(original.periods_in_transit),
		feeder_share(feeder_share),
		source_xy(original.source_xy),
		travelled(original.travelled),
		source(original.source),
		first_station(original.first_station),
		next_hop(original.next_hop)
{
	this->SetPassengerTicket(original.ticket);
	this->through_fare = original.through_fare;
	this->through_distance = original.through_distance;
	this->fare_shares = original.fare_shares;
	this->fare_interchange = original.fare_interchange;
	this->fare_mileage = original.fare_mileage;
	this->passenger_alight = original.passenger_alight;
	this->passenger_destination = original.passenger_destination;
	this->passenger_start_tick = original.passenger_start_tick;
	this->external_source_city = original.external_source_city;
	this->external_distance = original.external_distance;
	dbg_assert(count != 0);
#ifdef WITH_FULL_ASSERTS
	this->flags |= (original.flags & CPF_IN_VEHICLE);
#endif /* WITH_FULL_ASSERTS */
}

/** Destroy the packet. */
CargoPacket::~CargoPacket()
{
	if (CleaningPool()) return;
	if (this->ticket.train != VehicleID::Invalid()) this->SetPassengerTicket({});

	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		IterateCargoPacketDeferredPayments(this->index, true, [](Money &payment, CompanyID cid, VehicleType type) { });
	}
}

/**
 * Split this packet in two and return the split off part.
 * @param new_size Size of the split part.
 * @return Split off part, or nullptr if no packet could be allocated!
 */
CargoPacket *CargoPacket::Split(uint new_size)
{
	if (!CargoPacket::CanAllocateItem()) return nullptr;

	Money fs = this->GetFeederShare(new_size);
	CargoPacket *cp_new = CargoPacket::Create(new_size, fs, *this);
	this->feeder_share -= fs;

	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		std::vector<std::pair<uint64_t, Money>> to_add;
		IterateCargoPacketDeferredPayments(this->index, false, [&](Money &payment, CompanyID cid, VehicleType type) {
			Money share = payment * new_size / static_cast<uint>(this->count);
			payment -= share;
			to_add.push_back({ CargoPacketDeferredPaymentKey(cp_new->index, cid, type), share });
		});
		for (auto &m : to_add) {
			_cargo_packet_deferred_payments[m.first] = m.second;
		}
		cp_new->flags |= CPF_HAS_DEFERRED_PAYMENT;
	}

	this->count -= new_size;
	return cp_new;
}

/**
 * Merge another packet into this one.
 * @param cp Packet to be merged in.
 */
void CargoPacket::Merge(CargoPacket *cp)
{
	this->count += cp->count;
	this->feeder_share += cp->feeder_share;

	if (cp->flags & CPF_HAS_DEFERRED_PAYMENT) {
		std::vector<std::pair<uint64_t, Money>> to_merge;
		IterateCargoPacketDeferredPayments(cp->index, true, [&](Money &payment, CompanyID cid, VehicleType type) {
			to_merge.push_back({ CargoPacketDeferredPaymentKey(this->index, cid, type), payment });
		});
		cp->flags &= ~CPF_HAS_DEFERRED_PAYMENT;

		for (auto &m : to_merge) {
			_cargo_packet_deferred_payments[m.first] += m.second;
		}
		this->flags |= CPF_HAS_DEFERRED_PAYMENT;
	}

	delete cp;
}

/**
 * Reduce the packet by the given amount and remove the feeder share.
 * @param count Amount to be removed.
 */
void CargoPacket::Reduce(uint count)
{
	dbg_assert(count < this->count);
	this->feeder_share -= this->GetFeederShare(count);
	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		IterateCargoPacketDeferredPayments(this->index, false, [&](Money &payment, CompanyID cid, VehicleType type) {
			payment -= payment * count / static_cast<uint>(this->count);
		});
	}
	this->count -= count;
}

void CargoPacket::RegisterDeferredCargoPayment(CompanyID cid, VehicleType type, Money payment)
{
	this->flags |= CPF_HAS_DEFERRED_PAYMENT;
	_cargo_packet_deferred_payments[CargoPacketDeferredPaymentKey(this->index, cid, type)] += payment;
}

void CargoPacket::LockThroughFare(const Station &from, const Vehicle *carrier, StationID alight)
{
	if (!_settings_game.linkgraph.passenger_through_fares || this->through_fare >= 0 ||
			from.index != this->first_station || this->source_xy != INVALID_TILE) return;
	const Station *to = Station::GetIfValid(this->passenger_destination);
	if (to == nullptr || to == &from) return;
	if (this->ticket.quoted_fare >= 0) {
		this->through_fare = this->ticket.quoted_fare;
		this->through_distance = this->ticket.quoted_distance;
		this->fare_shares = this->ticket.quoted_shares;
		return;
	}
	/* A through ticket uses fixed city boundary references, including the off-map distance,
	 * even if a connecting service has not yet chosen a particular gateway. */
	const uint distance = PassengerFareDistance(from, *to);
	if (!carrier) carrier = Vehicle::GetIfValid(this->ticket.train);
	if (_settings_game.linkgraph.passenger_route_fares && carrier) {
		/* A walk-up may know only its first ride. Lock that ride and the
		 * remaining station distance now, without inventing a later itinerary. */
		const auto call = this->ticket.call != INVALID_VEH_ORDER_ID ? this->ticket.call : carrier->cur_real_order_index;
		if (alight == StationID::Invalid()) alight = this->ticket.alight;
		if (alight == StationID::Invalid()) alight = this->GetNextHop();
		if (alight == StationID::Invalid() && CanContinuePassengerLeg(*carrier, to->index, GetCargoTypeByLabel(CT_PASSENGERS))) alight = to->index;
		const Station *change = Station::GetIfValid(alight);
		const auto known = change ? PassengerBookedDistance(*carrier, call, from.index, alight) : std::nullopt;
		uint first = known.value_or(change ? PassengerFareDistance(from, *change) : distance);
		const uint rest = change && change != to ? PassengerFareDistance(*change, *to) : 0;
		if (!known && rest == 0) first = distance;
		this->through_distance = ClampTo<uint32_t>(uint64_t(first) + rest);
		const PassengerTariff defaults;
		const uint64_t initial = Company::Get(carrier->owner)->QuotePassengerFare(first, 0, carrier->passenger_tariff, carrier->orders ? &carrier->orders->passenger_tariff : nullptr);
		const uint64_t remainder = defaults.Price(this->through_distance, 0, defaults) - defaults.Price(first, 0, defaults);
		this->through_fare = ClampTo<int32_t>((initial + remainder + 50) / 100);
		if (rest && change) {
			const auto share = std::min<uint32_t>(this->through_fare, ClampTo<uint32_t>((initial + 50) / 100));
			this->fare_shares = {change->index.base(), share, to->index.base(), uint32_t(this->through_fare) - share};
		}
		return;
	}
	const Money price = GetTransportedGoodsIncome(1, distance, ClampTo<uint16_t>(distance * 16 / CARGO_AGING_TICKS), GetCargoTypeByLabel(CT_PASSENGERS));
	this->through_fare = ClampTo<int32_t>(std::max<int64_t>(0, price.base()));
	/* A walk-up or a CargoDist first hop can lack a complete bookable itinerary.
	 * Quote the known operator's portion and use the ordinary fare for the rest. */
	if (!carrier) return;
	const OrderList *plan = this->ticket.schedule != OrderListID::Invalid() ? OrderList::GetIfValid(this->ticket.schedule) : carrier->orders;
	const auto &company = Company::Get(carrier->owner)->passenger_tariff;
	if (!carrier->passenger_tariff.Active() && !(plan && plan->passenger_tariff.Active()) && !company.Active()) return;
	if (alight == StationID::Invalid()) alight = this->ticket.alight;
	if (alight == StationID::Invalid()) alight = this->GetNextHop();
	if (alight == StationID::Invalid() && CanContinuePassengerLeg(*carrier, to->index, GetCargoTypeByLabel(CT_PASSENGERS))) alight = to->index;
	const Station *change = Station::GetIfValid(alight);
	if (change == nullptr || change == &from) return;
	const uint64_t rate = Company::Get(carrier->owner)->QuotePassengerFare(distance, 0, carrier->passenger_tariff, plan ? &plan->passenger_tariff : nullptr);
	const uint64_t first = std::max(1U, DistanceManhattan(from.xy, change->xy));
	const uint64_t rest = (change == to) ? 0 : std::max(1U, DistanceManhattan(change->xy, to->xy));
	const uint64_t divisor = (first + rest) * 100;
	const uint32_t initial = ClampTo<int32_t>((rate * first + divisor / 2) / divisor);
	this->through_fare = ClampTo<int32_t>((rate * first + uint64_t(this->through_fare) * 100 * rest + divisor / 2) / divisor);
	this->fare_shares = {change->index.base(), initial};
	if (rest != 0) this->fare_shares.insert(this->fare_shares.end(), {to->index.base(), uint32_t(this->through_fare) - initial});
}

uint CargoPacket::FareLegDistance(TileIndex tile) const
{
	return this->fare_interchange == INVALID_TILE ? this->GetDistance(tile) : DistanceManhattan(this->fare_interchange, tile);
}

void CargoPacket::RecordFareTransfer(const Vehicle &carrier, TileIndex tile, StationID station)
{
	const uint distance = this->FareLegDistance(tile);
	this->fare_interchange = tile;
	const uint slot = carrier.owner.base() * 4 + to_underlying(carrier.type);
	if (!this->fare_shares.empty()) {
		/* Unfinished sections are in itinerary order. Completed portions carry
		 * the actual operator even if the remaining journey is rebooked. */
		uint32_t earned = 0;
		size_t end = 0;
		while (end < this->fare_shares.size() && this->fare_shares[end] < FARE_OPERATOR) {
			earned += this->fare_shares[end + 1];
			end += 2;
			if (this->fare_shares[end - 2] == station.base()) break;
		}
		if (end && this->fare_shares[end - 2] == station.base()) {
			this->fare_shares.erase(this->fare_shares.begin(), this->fare_shares.begin() + end);
		} else {
			earned = 0;
			if (end && distance != 0) {
				const Station *next = Station::GetIfValid(StationID{static_cast<uint16_t>(this->fare_shares[0])});
				if (next) {
					const uint remaining = DistanceManhattan(tile, next->xy);
					earned = uint64_t(this->fare_shares[1]) * distance / (uint64_t(distance) + remaining);
					this->fare_shares[1] -= earned;
				}
			}
		}
		for (size_t i = 0; earned && i < this->fare_shares.size(); i += 2) {
			if (this->fare_shares[i] != FARE_OPERATOR + slot) continue;
			this->fare_shares[i + 1] += earned;
			earned = 0;
		}
		if (earned) this->fare_shares.insert(this->fare_shares.end(), {FARE_OPERATOR + slot, earned});
	}
	if (distance == 0) return;
	if (slot >= this->fare_mileage.size()) this->fare_mileage.resize(slot + 1);
	this->fare_mileage[slot] = ClampTo<uint32_t>(uint64_t(this->fare_mileage[slot]) + distance);
}

/** Credit completed carriers once for the passengers actually delivered. Weights are per person,
 * so a split, partial unloading or a later delivery does not consume the other passengers' shares. */
Money CargoPacket::SettleThroughFare(uint accepted, const Vehicle &carrier, TileIndex tile, uint offmap) const
{
	Money remaining = Money(this->through_fare) * accepted;
	static constexpr ExpensesType revenues[] = {ExpensesType::TrainRevenue, ExpensesType::RoadVehRevenue, ExpensesType::ShipRevenue, ExpensesType::AircraftRevenue};
	const uint final_slot = carrier.owner.base() * 4 + to_underlying(carrier.type);
	if (!this->fare_shares.empty()) {
		for (size_t i = 0; i + 1 < this->fare_shares.size(); i += 2) {
			if (this->fare_shares[i] < FARE_OPERATOR) continue;
			const uint slot = this->fare_shares[i] - FARE_OPERATOR;
			if (slot == final_slot || slot >= MAX_COMPANIES * 4) continue;
			const CompanyID company{static_cast<uint8_t>(slot / 4)};
			if (!Company::IsValidID(company)) continue;
			const Money share = Money(this->fare_shares[i + 1]) * accepted;
			SubtractMoneyFromCompany(company, CommandCost(revenues[slot % 4], -share));
			remaining -= share;
		}
		return remaining;
	}
	if (this->fare_mileage.empty() || accepted == 0) return remaining;
	const uint64_t total = std::accumulate(this->fare_mileage.begin(), this->fare_mileage.end(), uint64_t(this->FareLegDistance(tile)) + offmap);
	if (total == 0) return remaining;
	for (uint slot = 0; slot < this->fare_mileage.size(); ++slot) {
		if (!this->fare_mileage[slot] || slot == final_slot) continue;
		const CompanyID company{static_cast<uint8_t>(slot / 4)};
		if (!Company::IsValidID(company)) continue;
		const Money share = Money(uint64_t(this->through_fare) * this->fare_mileage[slot] / total) * accepted;
		if (share == 0) continue;
		SubtractMoneyFromCompany(company, CommandCost(revenues[slot % 4], -share));
		remaining -= share;
	}
	return remaining;
}

void CargoPacket::PayDeferredPayments()
{
	if (this->flags & CPF_HAS_DEFERRED_PAYMENT) {
		IterateCargoPacketDeferredPayments(this->index, true, [&](Money &payment, CompanyID cid, VehicleType type) {
			ExpensesType exp;
			switch (type) {
				case VehicleType::Train: exp = ExpensesType::TrainRevenue; break;
				case VehicleType::Road: exp = ExpensesType::RoadVehRevenue; break;
				case VehicleType::Ship: exp = ExpensesType::ShipRevenue; break;
				case VehicleType::Aircraft: exp = ExpensesType::AircraftRevenue; break;
				default: NOT_REACHED();
			}
			SubtractMoneyFromCompany(cid, CommandCost(exp, -payment));
		});
		this->flags &= ~CPF_HAS_DEFERRED_PAYMENT;
	}
}

/**
 * Invalidates (sets source_id to INVALID_SOURCE) all cargo packets from given source.
 * @param src Index of source.
 */
/* static */ void CargoPacket::InvalidateAllFrom(Source src)
{
	for (CargoPacket *cp : CargoPacket::Iterate()) {
		if (cp->source == src) cp->source.MakeInvalid();
	}
}

/**
 * Invalidates (sets source to StationID::Invalid()) all cargo packets from given station.
 * @param sid Station that gets removed.
 */
/* static */ void CargoPacket::InvalidateAllFrom(StationID sid)
{
	for (CargoPacket *cp : CargoPacket::Iterate()) {
		if (cp->ticket.bus_via == sid || cp->ticket.alight == sid || cp->passenger_destination == sid) cp->SetPassengerTicket({});
		for (auto *shares : {&cp->fare_shares, &cp->ticket.quoted_shares}) {
			for (size_t i = 0; i < shares->size(); i += 2) {
				if ((*shares)[i] == sid.base()) (*shares)[i] = StationID::Invalid().base();
			}
		}
		if (cp->first_station == sid) cp->first_station = StationID::Invalid();
		if (cp->passenger_alight == sid) cp->passenger_alight = StationID::Invalid();
		if (cp->passenger_destination == sid) {
			cp->passenger_destination = StationID::Invalid();
			cp->through_fare = -1;
			cp->through_distance = 0;
			cp->fare_shares.clear();
			cp->fare_mileage.clear();
			cp->fare_interchange = INVALID_TILE;
		}
	}
}

/* static */ bool CargoPacket::ValidateDeferredCargoPayments()
{
	for (auto &it : _cargo_packet_deferred_payments) {
		uint id = it.first >> 32;
		const CargoPacket *cp = CargoPacket::GetIfValid(id);
		if (!cp) return false;
		if (!(cp->flags & CPF_HAS_DEFERRED_PAYMENT)) return false;
	}
	return true;
}

/*
 *
 * Cargo list implementation
 *
 */

/**
 * Destroy the cargolist ("frees" all cargo packets).
 */
template <class Tinst, class Tcont>
CargoList<Tinst, Tcont>::~CargoList()
{
	for (Iterator it(this->packets.begin()); it != this->packets.end(); ++it) {
		delete *it;
	}
}

/**
 * Empty the cargo list, but don't free the cargo packets;
 * the cargo packets are cleaned by CargoPacket's CleanPool.
 */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::OnCleanPool()
{
	this->packets.clear();
}

/**
 * Update the cached values to reflect the removal of this packet or part of it.
 * Decreases count and periods_in_transit.
 * @param cp Packet to be removed from cache.
 * @param count Amount of cargo from the given packet to be removed.
 */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::RemoveFromCache(const CargoPacket *cp, uint count)
{
	dbg_assert(count <= cp->count);
	this->count -= count;
	this->cargo_periods_in_transit -= static_cast<uint64_t>(cp->periods_in_transit) * count;
}

/**
 * Update the cache to reflect adding of this packet.
 * Increases count and periods_in_transit.
 * @param cp New packet to be inserted.
 */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::AddToCache(const CargoPacket *cp)
{
	this->count += cp->count;
	this->cargo_periods_in_transit += static_cast<uint64_t>(cp->periods_in_transit) * cp->count;
}

/** Invalidates the cached data and rebuilds it. */
template <class Tinst, class Tcont>
void CargoList<Tinst, Tcont>::InvalidateCache()
{
	this->count = 0;
	this->cargo_periods_in_transit = 0;

	for (ConstIterator it(this->packets.begin()); it != this->packets.end(); it++) {
		static_cast<Tinst *>(this)->AddToCache(*it);
	}
}

/**
 * Tries to merge the second packet into the first and return if that was
 * successful.
 * @param icp Packet to be merged into.
 * @param cp Packet to be eliminated.
 * @return If the packets could be merged.
 */
template <class Tinst, class Tcont>
/* static */ bool CargoList<Tinst, Tcont>::TryMerge(CargoPacket *icp, CargoPacket *cp)
{
	if (Tinst::AreMergable(icp, cp) &&
			icp->count + cp->count <= CargoPacket::MAX_COUNT) {
		icp->Merge(cp);
		return true;
	} else {
		return false;
	}
}

/*
 *
 * Vehicle cargo list implementation.
 *
 */

/**
 * Appends the given cargo packet. Tries to merge it with another one in the
 * packets list. If no fitting packet is found, appends it. You can only append
 * packets to the ranges of packets designated for keeping or loading.
 * Furthermore if there are already packets reserved for loading you cannot
 * directly add packets to the "keep" list. You first have to load the reserved
 * ones.
 * @warning After appending this packet may not exist anymore!
 * @note Do not use the cargo packet anymore after it has been appended to this CargoList!
 * @param cp Cargo packet to add.
 * @param action Either MoveToAction::Keep if you want to add the packet directly or MoveToAction::Load
 * if you want to reserve it first.
 * @pre cp != nullptr
 * @pre action == MoveToAction::Load || (action == MoveToAction::Keep && this->designation_counts[MoveToAction::Load] == 0)
 */
void VehicleCargoList::Append(CargoPacket *cp, MoveToAction action)
{
	dbg_assert(cp != nullptr);
	dbg_assert(action == MoveToAction::Load ||
			(action == MoveToAction::Keep && this->action_counts[MoveToAction::Load] == 0));
	this->AddToMeta(cp, action);

	if (this->count == cp->count) {
		this->packets.push_back(cp);
		return;
	}

	uint sum = cp->count;
	for (ReverseIterator it(this->packets.rbegin()); it != this->packets.rend(); it++) {
		CargoPacket *icp = *it;
		if (VehicleCargoList::TryMerge(icp, cp)) return;
		sum += icp->count;
		if (sum >= this->action_counts[action]) {
			this->packets.push_back(cp);
			return;
		}
	}

	NOT_REACHED();
}

/**
 * Shifts cargo from the front of the packet list and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 */
template <class Taction>
void VehicleCargoList::ShiftCargo(Taction action)
{
	Iterator it(this->packets.begin());
	while (it != this->packets.end() && action.MaxMove() > 0) {
		CargoPacket *cp = *it;
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			break;
		}
	}
}

/**
 * Shifts cargo from the front of the packet list and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *, std::vector<CargoPacket*> &)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 *                 The second method parameter can be appended to, to prepend items to the packet list
 * @param action Action instance to be applied.
 * @param filter Cargo packet filter.
 */
template <class Taction, class Tfilter>
void VehicleCargoList::ShiftCargoWithFrontInsert(Taction action, Tfilter filter)
{
	std::vector<CargoPacket *> packets_to_front_insert;

	Iterator it(this->packets.begin());
	while (it != this->packets.end() && action.MaxMove() > 0) {
		CargoPacket *cp = *it;
		if (!filter(cp)) {
			++it;
			continue;
		}
		if (action(cp, packets_to_front_insert)) {
			it = this->packets.erase(it);
		} else {
			break;
		}
	}

	for (CargoPacket *cp : packets_to_front_insert) {
		this->packets.push_front(cp);
	}
}

/**
 * Pops cargo from the back of the packet list and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 */
template <class Taction>
void VehicleCargoList::PopCargo(Taction action)
{
	if (this->packets.empty()) return;
	for (auto it = this->packets.end(); it != this->packets.begin();) {
		if (action.MaxMove() <= 0) break;
		--it;
		CargoPacket *cp = *it;
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			break;
		}
	}
}

void VehicleCargoList::AssertCountConsistencyError() const
{
	assert_msg(this->action_counts[MoveToAction::Keep] +
			this->action_counts[MoveToAction::Deliver] +
			this->action_counts[MoveToAction::Transfer] +
			this->action_counts[MoveToAction::Load] == this->count,
			"{} + {} + {} + {} != {}, ({} in {} packets)",
			this->action_counts[MoveToAction::Keep],
			this->action_counts[MoveToAction::Deliver],
			this->action_counts[MoveToAction::Transfer],
			this->action_counts[MoveToAction::Load],
			this->count,
			this->RecalculateCargoTotal(),
			this->packets.size());
}

/**
 * Update the cached values to reflect the removal of this packet or part of it.
 * Decreases count, feeder share and periods_in_transit.
 * @param cp Packet to be removed from cache.
 * @param count Amount of cargo from the given packet to be removed.
 */
void VehicleCargoList::RemoveFromCache(const CargoPacket *cp, uint count)
{
	this->feeder_share -= cp->GetFeederShare(count);
	this->Parent::RemoveFromCache(cp, count);
}

/**
 * Update the cache to reflect adding of this packet.
 * Increases count, feeder share and periods_in_transit.
 * @param cp New packet to be inserted.
 */
void VehicleCargoList::AddToCache(const CargoPacket *cp)
{
	this->feeder_share += cp->feeder_share;
	this->Parent::AddToCache(cp);
}

/**
 * Removes a packet or part of it from the metadata.
 * @param cp Packet to be removed.
 * @param action MoveToAction of the packet (for updating the counts).
 * @param count Amount of cargo to be removed.
 */
void VehicleCargoList::RemoveFromMeta(const CargoPacket *cp, MoveToAction action, uint count)
{
	dbg_assert(count <= this->action_counts[action]);
	this->AssertCountConsistency();
	this->RemoveFromCache(cp, count);
	this->action_counts[action] -= count;
	this->AssertCountConsistency();
}

/**
 * Adds a packet to the metadata.
 * @param cp Packet to be added.
 * @param action MoveToAction of the packet.
 */
void VehicleCargoList::AddToMeta(const CargoPacket *cp, MoveToAction action)
{
	this->AssertCountConsistency();
	this->AddToCache(cp);
	this->action_counts[action] += cp->count;
	this->AssertCountConsistency();
}

/**
 * Ages the all cargo in this list.
 */
void VehicleCargoList::AgeCargo()
{
	for (const auto &cp : this->packets) {
		/* If we're at the maximum, then we can't increase no more. */
		if (cp->periods_in_transit == UINT16_MAX) continue;

		cp->periods_in_transit++;
		this->cargo_periods_in_transit += cp->count;
	}
}

/**
 * Choose action to be performed with the given cargo packet.
 * @param cp The packet.
 * @param cargo_next Next hop the cargo wants to pass.
 * @param current_station Current station of the vehicle carrying the cargo.
 * @param accepted If the cargo is accepted at the current station.
 * @param next_station Next station(s) the vehicle may stop at.
 * @return MoveToAction to be performed.
 */
/* static */ VehicleCargoList::MoveToAction VehicleCargoList::ChooseAction(const CargoPacket *cp, StationID cargo_next,
		StationID current_station, bool accepted, std::span<const StationID> next_station)
{
	if (cargo_next == StationID::Invalid()) {
		return (accepted && cp->first_station != current_station) ? MoveToAction::Deliver : MoveToAction::Keep;
	} else if (cargo_next == current_station) {
		return MoveToAction::Deliver;
	} else if (std::ranges::find(next_station, cargo_next) != std::end(next_station)) {
		return MoveToAction::Keep;
	} else {
		return MoveToAction::Transfer;
	}
}

static StationID FindPassengerConnection(const CargoPacket &packet, StationID current, CargoType cargo)
{
	/* Follow unrestricted links for this origin, retaining the first hop of each reachable path. */
	std::vector<std::pair<StationID, StationID>> queue{{current, current}};
	std::set<StationID> seen{current};
	for (size_t i = 0; i < queue.size(); ++i) {
		auto [station, first] = queue[i];
		const auto &flows = Station::Get(station)->goods[cargo].ConstFlows();
		auto flow = flows.find(packet.GetFirstStation());
		if (flow == flows.end()) continue;
		for (const auto &share : *flow) {
			if (share.first > flow->GetUnrestricted()) break;
			StationID via = share.second;
			if (!Station::IsValidID(via) || !seen.insert(via).second) continue;
			StationID hop = i == 0 ? via : first;
			if (via == packet.passenger_destination) return hop;
			queue.emplace_back(via, hop);
		}
	}
	return StationID::Invalid();
}

/**
 * Stages cargo for unloading. The cargo is sorted so that packets to be
 * transferred, delivered or kept are in consecutive chunks in the list. At the
 * same time the designation_counts are updated to reflect the size of those
 * chunks.
 * @param accepted If the cargo will be accepted at the station.
 * @param current_station ID of the station.
 * @param next_station ID of the station the vehicle will go to next.
 * @param unload_type OrderUnloadType that will apply to the unload operation.
 * @param ge GoodsEntry for getting the flows.
 * @param cargo The cargo type of the cargo.
 * @param payment Payment object for registering transfers.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return \c true iff any cargo will be unloaded.
 */
bool VehicleCargoList::Stage(bool accepted, StationID current_station, std::span<const StationID> next_station, OrderUnloadType unload_type, const GoodsEntry *ge, CargoType cargo, CargoPayment *payment, TileIndex current_tile)
{
	this->AssertCountConsistency();
	dbg_assert(this->action_counts[MoveToAction::Load] == 0);
	this->action_counts[MoveToAction::Transfer] = this->action_counts[MoveToAction::Deliver] = this->action_counts[MoveToAction::Keep] = 0;
	Iterator it = this->packets.begin();
	uint sum = 0;
	CargoPacketList transfer_deliver;
	std::vector<CargoPacket *> keep;

	const FlowStatMap &flows = ge->ConstFlows();

	bool force_keep = (unload_type == OrderUnloadType::NoUnload);
	bool force_unload = (unload_type == OrderUnloadType::Unload);
	bool force_transfer = (unload_type == OrderUnloadType::Transfer) || (unload_type == OrderUnloadType::Unload);
	std::map<std::pair<StationID, StationID>, StationID> passenger_connections;
	bool transfer_cargodist_mode = force_transfer && _settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual;
	dbg_assert(this->count > 0 || it == this->packets.end());
	while (sum < this->count) {
		CargoPacket *cp = *it;

		it = this->packets.erase(it);
		StationID cargo_next = StationID::Invalid();
		MoveToAction action = MoveToAction::Load;

		auto handle_forced_transfer = [&]() {
			action = MoveToAction::Transfer;
			/* We cannot send the cargo to any of the possible next hops and
			 * also not to the current station. */
			FlowStatMap::const_iterator flow_it(flows.find(cp->first_station));
			if (flow_it == flows.end()) {
				cargo_next = StationID::Invalid();
			} else {
				FlowStat new_shares = *flow_it;
				new_shares.ChangeShare(current_station, INT_MIN);
				for (StationID st : next_station) {
					if (new_shares.empty()) break;
					new_shares.ChangeShare(st, INT_MIN);
				}
				if (new_shares.empty()) {
					cargo_next = StationID::Invalid();
				} else {
					cargo_next = new_shares.GetVia();
				}
			}
		};

		if (force_keep) {
			action = MoveToAction::Keep;
		} else if (cp->passenger_destination != StationID::Invalid()) {
			if (current_station == cp->passenger_destination && accepted && unload_type != OrderUnloadType::Transfer) {
				action = MoveToAction::Deliver;
			} else if (!force_transfer && cp->passenger_alight != StationID::Invalid() && cp->passenger_alight != current_station &&
					payment->front != nullptr && CanContinuePassengerLeg(*payment->front, cp->passenger_alight, cargo)) {
				action = MoveToAction::Keep;
			} else {
				auto [connection, inserted] = passenger_connections.try_emplace({cp->first_station, cp->passenger_destination}, StationID::Invalid());
				if (inserted) connection->second = FindPassengerConnection(*cp, current_station, cargo);
				cargo_next = connection->second;
				/* If the links have disappeared, wait for a service towards the intended station. */
				if (cargo_next == StationID::Invalid()) cargo_next = cp->passenger_destination;
				bool continues = std::ranges::find(next_station, cargo_next) != next_station.end();
				action = (cp->passenger_alight != current_station && !force_transfer && continues) ? MoveToAction::Keep : MoveToAction::Transfer;
			}
		} else if (force_unload && !transfer_cargodist_mode && accepted && cp->first_station != current_station) {
			action = MoveToAction::Deliver;
		} else if (force_transfer && !transfer_cargodist_mode) {
			handle_forced_transfer();
		} else {
			/* Rewrite an invalid source station to some random other one to
			 * avoid keeping the cargo in the vehicle forever. */
			if (cp->first_station == StationID::Invalid() && !flows.empty()) {
				cp->first_station = flows.FirstStationID();
			}
			bool restricted = false;
			FlowStatMap::const_iterator flow_it(flows.find(cp->first_station));
			if (flow_it == flows.end()) {
				cargo_next = StationID::Invalid();
			} else {
				cargo_next = flow_it->GetViaWithRestricted(restricted);
			}
			action = VehicleCargoList::ChooseAction(cp, cargo_next, current_station, accepted, next_station);
			if (restricted && action == MoveToAction::Transfer) {
				/* If the flow is restricted we can't transfer to it. Choose an
				 * unrestricted one instead. */
				cargo_next = flow_it->GetVia();
				action = VehicleCargoList::ChooseAction(cp, cargo_next, current_station, accepted, next_station);
			}
			if (transfer_cargodist_mode && action == MoveToAction::Keep) {
				handle_forced_transfer();
			}
		}
		Money share;
		switch (action) {
			case MoveToAction::Keep:
				keep.push_back(cp);
				break;
			case MoveToAction::Deliver:
				transfer_deliver.push_back(cp);
				break;
			case MoveToAction::Transfer:
				transfer_deliver.push_front(cp);
				/* Add feeder share here to allow reusing field for next station. */
				share = payment->PayTransfer(cargo, cp, cp->count, current_tile);
				cp->AddFeederShare(share);
				this->feeder_share += share;
				cp->next_hop = cargo_next;
				break;
			default:
				NOT_REACHED();
		}
		this->action_counts[action] += cp->count;
		sum += cp->count;
	}
	dbg_assert(this->packets.empty());
	this->packets = std::move(transfer_deliver);
	this->packets.insert(this->packets.end(), keep.begin(), keep.end());
	this->AssertCountConsistency();
	return this->action_counts[MoveToAction::Deliver] > 0 || this->action_counts[MoveToAction::Transfer] > 0;
}

/** Invalidates the cached data and rebuild it. */
void VehicleCargoList::InvalidateCache()
{
	this->feeder_share = 0;
	this->Parent::InvalidateCache();
}

/**
 * Moves some cargo from one designation to another. You can only move
 * between adjacent designations. E.g. you can keep cargo that was previously
 * reserved (MoveToAction::Load), but you can't reserve cargo that's marked as to be
 * delivered. Furthermore, as this method doesn't change the actual packets,
 * you cannot move cargo from or to MoveToAction::Transfer. You need a specialized
 * template method for that.
 * @tparam from Previous designation of cargo.
 * @tparam to New designation of cargo.
 * @param max_move Maximum amount of cargo to reassign.
 * @return Amount of cargo actually reassigned.
 */
template <VehicleCargoList::MoveToAction Tfrom, VehicleCargoList::MoveToAction Tto>
uint VehicleCargoList::Reassign(uint max_move)
{
	static_assert(Tfrom != MoveToAction::Transfer && Tto != MoveToAction::Transfer);
	static_assert(to_underlying(Tfrom) - to_underlying(Tto) == 1 || to_underlying(Tto) - to_underlying(Tfrom) == 1);
	max_move = std::min(this->action_counts[Tfrom], max_move);
	this->action_counts[Tfrom] -= max_move;
	this->action_counts[Tto] += max_move;
	return max_move;
}

/**
 * Reassign cargo from MoveToAction::Deliver to MoveToAction::Transfer and take care of the next
 * station the cargo wants to visit.
 * @param max_move Maximum amount of cargo to reassign.
 * @return Amount of cargo actually reassigned.
 */
template <>
uint VehicleCargoList::Reassign<VehicleCargoList::MoveToAction::Deliver, VehicleCargoList::MoveToAction::Transfer>(uint max_move)
{
	max_move = std::min(this->action_counts[MoveToAction::Deliver], max_move);

	uint sum = 0;
	for (Iterator it(this->packets.begin()); sum < this->action_counts[MoveToAction::Transfer] + max_move;) {
		CargoPacket *cp = *it++;
		sum += cp->Count();
		if (sum <= this->action_counts[MoveToAction::Transfer]) continue;
		if (sum > this->action_counts[MoveToAction::Transfer] + max_move) {
			CargoPacket *cp_split = cp->Split(sum - this->action_counts[MoveToAction::Transfer] + max_move);
			sum -= cp_split->Count();
			it = this->packets.insert(it, cp_split);
			/* it points to the inserted value, which is just before the previous value of it.
			 * Increment it so that it points to the same element as it did before the insert.
			 */
			++it;
		}
		cp->next_hop = StationID::Invalid();
	}

	this->action_counts[MoveToAction::Deliver] -= max_move;
	this->action_counts[MoveToAction::Transfer] += max_move;
	return max_move;
}

/**
 * Returns reserved cargo to the station and removes it from the cache.
 * @param max_move Maximum amount of cargo to move.
 * @param dest Station the cargo is returned to.
 * @param next ID of the next station the cargo wants to go to.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return Amount of cargo actually returned.
 */
uint VehicleCargoList::Return(uint max_move, StationCargoList *dest, StationID next, TileIndex current_tile)
{
	max_move = std::min(this->action_counts[MoveToAction::Load], max_move);
	this->PopCargo(CargoReturn(this, dest, max_move, next, current_tile));
	return max_move;
}

/**
 * Shifts cargo between two vehicles.
 * @param dest Other vehicle's cargo list.
 * @param max_move Maximum amount of cargo to be moved.
 * @return Amount of cargo actually moved.
 */
uint VehicleCargoList::Shift(uint max_move, VehicleCargoList *dest)
{
	max_move = std::min(this->count, max_move);
	this->PopCargo(CargoShift(this, dest, max_move));
	return max_move;
}

/**
 * Unloads cargo at the given station. Deliver or transfer, depending on the
 * ranges defined by designation_counts.
 * @param dest StationCargoList to add transferred cargo to.
 * @param max_move Maximum amount of cargo to move.
 * @param cargo The cargo type of the cargo.
 * @param payment Payment object to register payments in.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return Amount of cargo actually unloaded.
 */
uint VehicleCargoList::Unload(uint max_move, StationCargoList *dest, CargoType cargo, CargoPayment *payment, TileIndex current_tile)
{
	uint moved = 0;
	if (this->action_counts[MoveToAction::Transfer] > 0) {
		uint move = std::min(this->action_counts[MoveToAction::Transfer], max_move);
		this->ShiftCargo(CargoTransfer(this, dest, move, current_tile, payment->front, payment->current_station));
		moved += move;
	}
	if (this->action_counts[MoveToAction::Transfer] == 0 && this->action_counts[MoveToAction::Deliver] > 0 && moved < max_move) {
		uint move = std::min(this->action_counts[MoveToAction::Deliver], max_move - moved);
		this->ShiftCargo(CargoDelivery(this, move, cargo, payment, current_tile));
		moved += move;
	}
	return moved;
}

/**
 * Truncates the cargo in this list to the given amount. It leaves the
 * first cargo entities and removes max_move from the back of the list.
 * @param max_move Maximum amount of entities to be removed from the list.
 * @return Amount of entities actually moved.
 */
uint VehicleCargoList::Truncate(uint max_move)
{
	max_move = std::min(this->count, max_move);
	if (max_move > this->ActionCount(MoveToAction::Keep)) this->KeepAll();
	this->PopCargo(CargoRemoval<VehicleCargoList>(this, max_move));
	return max_move;
}

uint VehicleCargoList::DeliverToExternal(uint max_move, CargoType cargo, TileIndex tile, uint16_t city, uint16_t distance, Money &income, StationID destination, const GoodsEntry *goods, const Vehicle *carrier)
{
	if (this->ReservedCount() != 0) return 0;
	this->KeepAll();
	uint delivered = 0;
	for (auto it = this->packets.begin(); it != this->packets.end() && delivered < max_move;) {
		CargoPacket *original = *it;
		if (original->external_source_city == city) { ++it; continue; }
		const bool fixed = original->through_fare >= 0 || (cargo == GetCargoTypeByLabel(CT_PASSENGERS) && original->passenger_destination != StationID::Invalid() && _settings_game.linkgraph.AssignPassengerDestinations());
		if (original->through_fare >= 0 && carrier == nullptr) { ++it; continue; }
		if (fixed && original->passenger_destination != destination) { ++it; continue; }
		if (!fixed && goods != nullptr && _settings_game.linkgraph.GetDistributionType(cargo) != DistributionType::Manual) {
			const auto &flows = goods->ConstFlows();
			auto flow = flows.find(original->GetFirstStation());
			if (flow != flows.end()) {
				const StationID via = flow->GetVia();
				if (via != StationID::Invalid() && via != destination) { ++it; continue; }
			}
		}
		const uint amount = std::min<uint>(original->Count(), max_move - delivered);
		const bool whole = amount == original->Count();
		CargoPacket *piece = whole ? original : original->Split(amount);
		if (piece == nullptr) { ++it; continue; }
		if (piece->through_fare >= 0) {
			income += piece->SettleThroughFare(amount, *carrier, tile, distance);
		} else {
			income += GetTransportedGoodsIncome(amount, piece->GetDistance(tile) + distance, piece->GetPeriodsInTransit(), cargo) - piece->GetFeederShare();
			piece->PayDeferredPayments();
		}
		this->RemoveFromMeta(piece, MoveToAction::Keep, amount);
		delete piece;
		if (whole) it = this->packets.erase(it); else ++it;
		delivered += amount;
	}
	return delivered;
}

/**
 * Routes packets with station "avoid" as next hop to a different place.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to prepend the cargo to.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 * @return The number of elements that got rerouted.
 */
uint VehicleCargoList::Reroute(uint max_move, VehicleCargoList *dest, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	max_move = std::min(this->action_counts[MoveToAction::Transfer], max_move);
	this->ShiftCargoWithFrontInsert(VehicleCargoReroute(this, dest, max_move, avoid, avoid2, ge), [](CargoPacket *cp) { return true; });
	return max_move;
}

/**
 * Routes packets with station "avoid" as next hop to a different place, for a specific source station.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to prepend the cargo to.
 * @param source Source station.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 */
uint VehicleCargoList::RerouteFromSource(uint max_move, VehicleCargoList *dest, StationID source, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	max_move = std::min(this->action_counts[MoveToAction::Transfer], max_move);
	this->ShiftCargoWithFrontInsert(VehicleCargoReroute(this, dest, max_move, avoid, avoid2, ge), [source](CargoPacket *cp) { return cp->GetFirstStation() == source; });
	return max_move;
}

/*
 *
 * Station cargo list implementation.
 *
 */

/**
 * Appends the given cargo packet to the range of packets with the same next station
 * @warning After appending this packet may not exist anymore!
 * @note Do not use the cargo packet anymore after it has been appended to this CargoList!
 * @param next the next hop
 * @param cp the cargo packet to add
 * @pre cp != nullptr
 */
void StationCargoList::Append(CargoPacket *cp, StationID next)
{
	dbg_assert(cp != nullptr);
	this->AddToCache(cp);

	StationCargoPacketMap::List &list = this->packets[next];
	for (StationCargoPacketMap::List::reverse_iterator it(list.rbegin());
			it != list.rend(); it++) {
		if (StationCargoList::TryMerge(*it, cp)) return;
	}

	/* The packet could not be merged with another one */
	list.push_back(cp);
}

/** Follow native, origin-specific unrestricted shares to local consumption. */
static StationID TraceWaitingPassenger(StationID source, StationID station, StationID next, CargoType cargo, Randomizer &random)
{
	std::set<StationID> visited{source, station};
	while (Station::IsValidID(next) && visited.insert(next).second) {
		const auto &flows = Station::Get(next)->goods[cargo].ConstFlows();
		const auto flow = flows.find(source);
		if (flow == flows.end() || flow->IsInvalid() || flow->GetUnrestricted() == 0) break;
		const StationID following = flow->upper_bound(random.Next(flow->GetUnrestricted()))->second;
		if (following == next) return next;
		next = following;
	}
	return StationID::Invalid();
}

/** Assign a destination only after the whole path is known. Return the next hop for the native loading queue. */
static StationID RetryWaitingPassenger(CargoPacket &packet, const Station &station, CargoType cargo, StationID via, uint64_t round)
{
	if (packet.passenger_destination != StationID::Invalid() || packet.ticket.train != VehicleID::Invalid()) return via;
	const StationID source = packet.GetFirstStation();
	if (!Station::IsValidID(source)) return via;
	const auto &flows = station.goods[cargo].ConstFlows();
	const auto flow = flows.find(source);
	if (flow == flows.end() || flow->IsInvalid() || flow->GetUnrestricted() == 0) return via;
	Randomizer random;
	random.SetSeed(SimpleHash32(packet.index.base() ^ SimpleHash32(static_cast<uint32_t>(round))));
	StationID next = via;
	StationID dest = StationID::Invalid();
	if (std::ranges::any_of(*flow, [&](const auto &share) { return share.second == via && share.first <= flow->GetUnrestricted(); })) {
		dest = TraceWaitingPassenger(source, station.index, via, cargo, random);
	}
	if (dest == StationID::Invalid()) {
		next = flow->upper_bound(random.Next(flow->GetUnrestricted()))->second;
		if (next == via) return via;
		dest = TraceWaitingPassenger(source, station.index, next, cargo, random);
	}
	if (dest == StationID::Invalid()) return via;
	packet.passenger_destination = dest;
	if (packet.passenger_start_tick == 0) packet.passenger_start_tick = _scaled_tick_counter;
	return next;
}

/** Rotate through actual waiting packets, regardless of empty via buckets. The saved clock
 * selects the batch, so no cursor needs saving or rebuilding when a client joins. */
void StationCargoList::RetryPassengerDestinations(const Station &station, CargoType cargo, uint64_t round)
{
	if (this->AvailableCount() == 0 || station.goods[cargo].ConstFlows().empty()) return;
	constexpr size_t batch = 32;
	const size_t total = this->packets.size();
	if (total == 0) return;
	size_t skip = (round % CeilDiv(total, batch)) * batch;
	size_t left = batch;
	std::vector<std::pair<StationID, CargoPacket *>> moved;
	for (auto it = this->packets.begin(); it != this->packets.end() && left != 0;) {
		auto &[via, list] = *it;
		if (skip >= list.size()) {
			skip -= list.size();
			++it;
			continue;
		}
		const size_t take = std::min(left, list.size() - skip);
		size_t index = skip;
		for (size_t i = 0; i < take; ++i) {
			CargoPacket *packet = list[index];
			const StationID next = RetryWaitingPassenger(*packet, station, cargo, via, round);
			if (next == via) {
				++index;
			} else {
				moved.emplace_back(next, packet);
				list.erase(list.begin() + index);
			}
		}
		if (list.empty()) it = this->packets.Map::erase(it);
		else ++it;
		left -= take;
		skip = 0;
	}
	/* Only the via key changes. Counts, packet identities and transit ages stay intact. */
	for (const auto &[next, packet] : moved) this->packets.Insert(next, packet);
}

/**
 * Shifts cargo from the front of the packet list for a specific station and
 * applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param next Next hop the cargo wants to visit.
 * @return True if all packets with the given next hop have been removed,
 *         False otherwise.
 */
template <class Taction>
bool StationCargoList::ShiftCargo(Taction &action, StationID next)
{
	if constexpr (std::is_base_of_v<CargoLoad, Taction>) action.fare_hop = next;
	for (Iterator it = this->packets.lower_bound(next); it != this->packets.end() && it.GetKey() == next;) {
		if (action.MaxMove() == 0) return false;
		CargoPacket *cp = *it;
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			return false;
		}
	}
	return true;
}

/**
 * Shifts cargo from the front of the packet list for a specific station and
 * and optional also from the list for "any station", then applies some action
 * to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param next Next hop the cargo wants to visit.
 * @param include_invalid If cargo from the StationID::Invalid() list should be
 *                        used if necessary.
 * @return Amount of cargo actually moved.
 */
template <class Taction>
uint StationCargoList::ShiftCargo(Taction action, std::span<const StationID> next, bool include_invalid)
{
	uint max_move = action.MaxMove();
	for (StationID st : next) {
		this->ShiftCargo(action, st);
		if (action.MaxMove() == 0) break;
	}
	if (include_invalid && action.MaxMove() > 0) {
		this->ShiftCargo(action, StationID::Invalid());
	}
	return max_move - action.MaxMove();
}

/**
 * Shifts cargo from the front of the packet list for a specific station
 * from a specific source station and applies some action to it.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param source Source station.
 * @param next Next hop the cargo wants to visit.
 * @return True if all packets with the given next hop have been removed,
 *         False otherwise.
 */
template <class Taction>
bool StationCargoList::ShiftCargoFromSource(Taction &action, StationID source, StationID next)
{
	for (Iterator it = this->packets.lower_bound(next); it != this->packets.end() && it.GetKey() == next;) {
		if (action.MaxMove() == 0) return false;
		CargoPacket *cp = *it;
		if (cp->GetFirstStation() != source) {
			++it;
			continue;
		}
		if (action(cp)) {
			it = this->packets.erase(it);
		} else {
			return false;
		}
	}
	return true;
}

/**
 * Shifts cargo from the front of the packet list for a specific station
 * and optional also from the list for "any station", then applies some action
 * to it, for a specific source station.
 * @tparam Taction Action class or function to be used. It should define
 *                 "bool operator()(CargoPacket *)". If true is returned the
 *                 cargo packet will be removed from the list. Otherwise it
 *                 will be kept and the loop will be aborted.
 * @param action Action instance to be applied.
 * @param source Source station.
 * @param next Next hop the cargo wants to visit.
 * @param include_invalid If cargo from the StationID::Invalid() list should be
 *                        used if necessary.
 * @return Amount of cargo actually moved.
 */
template <class Taction>
uint StationCargoList::ShiftCargoFromSource(Taction action, StationID source, std::span<const StationID> next, bool include_invalid)
{
	uint max_move = action.MaxMove();
	for (StationID st : next) {
		this->ShiftCargoFromSource(action, source, st);
		if (action.MaxMove() == 0) break;
	}
	if (include_invalid && action.MaxMove() > 0) {
		this->ShiftCargoFromSource(action, source, StationID::Invalid());
	}
	return max_move - action.MaxMove();
}

uint StationCargoList::AvailableViaCount(StationID next) const
{
	uint count = 0;
	for (ConstIterator it = this->packets.lower_bound(next); it != this->packets.end() && it.GetKey() == next; ++it) {
		count += (*it)->count;
	}
	return count;
}

/**
 * Truncates where each destination loses roughly the same percentage of its
 * cargo. This is done by randomizing the selection of packets to be removed.
 * Optionally count the cargo by origin station.
 * @param max_move Maximum amount of cargo to remove.
 * @param cargo_per_source Container for counting the cargo by origin.
 * @return Amount of cargo actually moved.
 */
uint StationCargoList::Truncate(uint max_move, StationCargoAmountMap *cargo_per_source)
{
	max_move = std::min(max_move, this->count);
	uint prev_count = this->count;
	uint moved = 0;
	uint loop = 0;
	bool do_count = cargo_per_source != nullptr;
	while (max_move > moved) {
		for (Iterator it(this->packets.begin()); it != this->packets.end();) {
			CargoPacket *cp = *it;
			if (prev_count > max_move && RandomRange(prev_count) < prev_count - max_move) {
				if (do_count && loop == 0) {
					(*cargo_per_source)[cp->first_station] += cp->count;
				}
				++it;
				continue;
			}
			uint diff = max_move - moved;
			if (cp->count > diff) {
				if (diff > 0) {
					this->RemoveFromCache(cp, diff);
					cp->Reduce(diff);
					moved += diff;
				}
				if (loop > 0) {
					if (do_count) (*cargo_per_source)[cp->first_station] -= diff;
					return moved;
				} else {
					if (do_count) (*cargo_per_source)[cp->first_station] += cp->count;
					++it;
				}
			} else {
				it = this->packets.erase(it);
				if (do_count && loop > 0) {
					(*cargo_per_source)[cp->first_station] -= cp->count;
				}
				moved += cp->count;
				this->RemoveFromCache(cp, cp->count);
				delete cp;
			}
		}
		loop++;
	}
	return moved;
}

/** Splitting only changes packet boundaries. The station's cached count and age
 * already include every traveller, so append directly to the same via bucket. */
void StationCargoList::SellPassengerSeats(PassengerBoarding &sale)
{
	struct Request {
		CargoPacket *packet;
		CargoPacketList *bucket;
		uint64_t priority;
		uint rail_distance = 0;
	};
	std::vector<Request> requests, rebookings;
	const TileIndex from = Station::Get(sale.station)->xy;
	for (auto &[via, bucket] : this->packets) {
		for (size_t i = 0; i < bucket.size(); ++i) {
			CargoPacket *packet = bucket[i];
			const bool had_ticket = packet->ticket.train != VehicleID::Invalid();
			if (had_ticket) {
				if (CargoPacket *rest = sale.CheckTicket(*packet, true); rest != nullptr) bucket.push_back(rest);
				if (packet->ticket.train != VehicleID::Invalid()) continue;
			}
			const Station *destination = Station::GetIfValid(packet->passenger_destination);
			if (destination == nullptr || destination->index == sale.station) continue;
			(had_ticket ? rebookings : requests).push_back({packet, &bucket, packet->passenger_start_tick});
		}
	}
	/* Release invalid tickets together before comparing itineraries. Otherwise a full
	 * section repeatedly opens and closes while adjacent packets are rebooked. */
	for (const Request &request : rebookings) {
		if (CargoPacket *rest = sale.CheckTicket(*request.packet); rest != nullptr) {
			request.bucket->push_back(rest);
			requests.push_back({rest, request.bucket, request.priority});
		}
	}
	if (requests.size() > 1) {
		/* Prioritise the rail leg being bought. An onward bus ride or a circuitous
		 * train path does not make this rail leg longer. Bound the head start so
		 * earlier short-distance travellers can still buy seats. */
		/* No seats change hands during this sorting pass. Groups with identical
		 * routing inputs can reuse the priority distance until actual sales begin. */
		robin_hood::unordered_flat_map<uint64_t, uint> distances;
		for (Request &request : requests) {
			const CargoPacket &p = *request.packet;
			const uint64_t key = (uint64_t{p.GetFirstStation().base()} << 18) | (uint64_t{p.passenger_destination.base()} << 2) |
					(uint64_t{p.through_fare >= 0} << 1) | (p.ticket.rebooking != PassengerRebooking::None);
			auto [entry, fresh] = distances.try_emplace(key, 0);
			if (fresh) {
				const auto &leg = sale.NextLeg(p);
				if (leg.train != VehicleID::Invalid()) entry->second = DistanceManhattan(from, Station::Get(leg.alight)->xy);
			}
			request.rail_distance = entry->second;
			const uint near = std::min(128U, request.rail_distance);
			const uint bonus = near + std::min(128U, (request.rail_distance - near) / 4);
			request.priority += 256 - bonus;
		}
		std::stable_sort(requests.begin(), requests.end(), [](const Request &a, const Request &b) {
			if (a.priority != b.priority) return a.priority < b.priority;
			return a.rail_distance > b.rail_distance;
		});
	}
	for (size_t i = 0; i < requests.size(); ++i) {
		const Request request = requests[i];
		if (CargoPacket *rest = sale.CheckTicket(*request.packet); rest != nullptr) {
			request.bucket->push_back(rest);
			requests.push_back({rest, request.bucket, request.priority});
		}
	}
}

/** Compare services for each fixed destination, including passengers waiting under a different next-hop key. */
template <class Taction>
uint StationCargoList::BoardCargo(Taction action, std::span<const StationID> next, const Vehicle *vehicle, PassengerBoarding *boarding)
{
	if (boarding == nullptr || vehicle == nullptr || !_settings_game.linkgraph.passenger_ticketing ||
			vehicle->cargo_type != GetCargoTypeByLabel(CT_PASSENGERS) ||
			_settings_game.linkgraph.GetDistributionType(vehicle->cargo_type) == DistributionType::Manual) return this->ShiftCargo(action, next, true);

	const Vehicle *front = vehicle->Primary();
	if (front == nullptr) return this->ShiftCargo(action, next, true);
	if (!IsPassengerTerminal(*Station::Get(boarding->station))) {
		/* Standalone docks and airports use the native loading queue. Price only
		 * the group that actually gets space, using its assigned CargoDist hop. */
		if (_settings_game.linkgraph.passenger_through_fares) {
			action.walkup_station = boarding->station;
			action.walkup_carrier = front;
		}
		return this->ShiftCargo(action, next, true);
	}
	const uint initial = action.MaxMove();
	std::vector<std::pair<StationID, CargoPacket *>> splits;
	for (Iterator it = this->packets.begin(); it != this->packets.end() && action.MaxMove() > 0;) {
		CargoPacket *packet = *it;
		action.alight = StationID::Invalid();
		/* A different vehicle cannot honour this ticket. Periodic station sales
		 * and explicit timetable edits revalidate it even if this vehicle leaves. */
		if (packet->ticket.train != VehicleID::Invalid() && packet->ticket.train != front->index) { ++it; continue; }
		/* A bus offers places on arrival, without running a station-wide rail ticket
		 * sale. Reconsider a different bus when it actually serves this destination. */
		const bool bus_offer = front->type == VehicleType::Road && packet->ticket.train == VehicleID::Invalid() &&
				std::ranges::find(next, packet->passenger_destination) != next.end();
		/* A reviewed transfer can be waiting when seats reopen. Check at actual
		 * boarding if this departure serves its next hop. The ordinary planner
		 * must still prefer this exact train and call over all alternatives. */
		bool onward_offer = false;
		if (front->type == VehicleType::Train && packet->ticket.reviewed && packet->ticket.rebooking == PassengerRebooking::Connection &&
				Station::IsValidID(packet->passenger_destination) && PassengerLoadingOpen(*front) &&
				(std::ranges::find(next, it.GetKey()) != next.end() || std::ranges::find(next, packet->passenger_destination) != next.end())) {
			const auto &leg = boarding->NextLeg(*packet);
			onward_offer = leg.train == front->index && leg.visit == 0 && leg.call == front->cur_real_order_index;
		}
		if (onward_offer || bus_offer || !packet->ticket.reviewed || packet->ticket.train != VehicleID::Invalid()) {
			if (CargoPacket *rest = boarding->CheckTicket(*packet); rest != nullptr) splits.emplace_back(it.GetKey(), rest);
		}
		const PassengerTicket &ticket = packet->ticket;
		/* A missed ticket needs a new verified rail leg before boarding another train.
		 * A bus, ship or aircraft can still serve a known CargoDist next hop. */
		if (ticket.rebooking == PassengerRebooking::Bus) {
			if (!CanBoardPassengerBus(*front, ticket, vehicle->cargo_type, ticket.alight == packet->passenger_destination)) { ++it; continue; }
			action.alight = ticket.alight;
			const uint take = std::min<uint>(packet->Count(), action.MaxMove());
			const uint room = boarding->inventory.Room(front->index, front->cur_real_order_index, 0, ticket.alight);
			boarding->inventory.Use(front->index, front->cur_real_order_index, 0, ticket.alight, take);
			if (room <= take) boarding->InvalidateSeatChoices();
		} else if (ticket.train == VehicleID::Invalid() && ticket.rebooking != PassengerRebooking::None) {
			/* A pending group can only use a vehicle going to its assigned next hop.
			 * Check that before looking up seats. A through ticket already has a
			 * locked fare, just as when NextLeg checks the same connection. */
			if (front->type == VehicleType::Train || it.GetKey() == StationID::Invalid() ||
					std::ranges::find(next, it.GetKey()) == next.end() ||
					(front->type == VehicleType::Road && (ticket.rebooking == PassengerRebooking::Full ||
					(Station::IsValidID(packet->passenger_destination) &&
					boarding->TicketFor(packet->passenger_destination, false, packet->through_fare >= 0).rebooking == PassengerRebooking::Full)))) {
				++it;
				continue;
			}
		}
		if (ticket.train != VehicleID::Invalid()) {
			if (ticket.visit != 0 || front->index != ticket.train || front->orders == nullptr || front->orders->index != ticket.schedule ||
					front->cur_real_order_index != ticket.call || !IsPassengerBoardingAt(*front, boarding->station) ||
					!PassengerLoadingOpen(*front) ||
					front->GetActiveServiceNumber() != ticket.number) {
				++it;
				continue;
			}
			action.alight = ticket.alight;
		}
		if (action.alight == StationID::Invalid() && it.GetKey() != StationID::Invalid() && std::ranges::find(next, it.GetKey()) == next.end()) { ++it; continue; }
		if (ticket.train == VehicleID::Invalid() && front->type == VehicleType::Train &&
				front->cur_real_order_index < front->GetNumOrders()) {
			/* A walk-up may use space left after all sold tickets, including downstream boardings. */
			const StationID stop = it.GetKey();
			const auto alight = packet->passenger_destination;
			const uint room = boarding->inventory.Room(front->index, front->cur_real_order_index, 0, alight);
			const uint take = std::min<uint>(packet->Count(), action.MaxMove());
			if (room == 0) { ++it; continue; }
			if (room < take) {
				CargoPacket *rest = packet->Split(packet->Count() - room);
				if (rest == nullptr) { ++it; continue; }
				splits.emplace_back(stop, rest);
			}
			boarding->inventory.Use(front->index, front->cur_real_order_index, 0, alight, std::min(take, room));
		}
		action.walkup_carrier = front;
		action.fare_hop = action.alight != StationID::Invalid() || front->type == VehicleType::Train ? action.alight : it.GetKey();
		action.walkup_station = ticket.train == VehicleID::Invalid() && _settings_game.linkgraph.passenger_through_fares ? boarding->station : StationID::Invalid();
		if (action(*it)) it = this->packets.erase(it);
		else break;
	}
	for (const auto &[via, rest] : splits) this->packets.Insert(via, rest);
	return initial - action.MaxMove();
}

/**
 * Reserves cargo for loading onto the vehicle.
 * @param max_move Maximum amount of cargo to reserve.
 * @param dest VehicleCargoList to reserve for.
 * @param next_station Next station(s) the loading vehicle will visit.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return Amount of cargo actually reserved.
 */
uint StationCargoList::Reserve(uint max_move, VehicleCargoList *dest, std::span<const StationID> next_station, TileIndex current_tile, const Vehicle *vehicle, PassengerBoarding *boarding)
{
	return this->BoardCargo(CargoReservation(this, dest, max_move, current_tile), next_station, vehicle, boarding);
}

/**
 * Loads cargo onto a vehicle. If the vehicle has reserved cargo load that.
 * Otherwise load cargo from the station.
 * @param max_move Amount of cargo to load.
 * @param dest Vehicle cargo list where the cargo resides.
 * @param next_station Next station(s) the loading vehicle will visit.
 * @param current_tile Current tile the cargo handling is happening on.
 * @return Amount of cargo actually loaded.
 * @note Vehicles may or may not reserve, depending on their orders. The two
 *       modes of loading are exclusive, though. If cargo is reserved we don't
 *       need to load unreserved cargo.
 */
uint StationCargoList::Load(uint max_move, VehicleCargoList *dest, std::span<const StationID> next_station, TileIndex current_tile, const Vehicle *vehicle, PassengerBoarding *boarding)
{
	uint move = std::min(dest->ActionCount(VehicleCargoList::MoveToAction::Load), max_move);
	if (move > 0) {
		this->reserved_count -= move;
		dest->Reassign<VehicleCargoList::MoveToAction::Load, VehicleCargoList::MoveToAction::Keep>(move);
		return move;
	} else {
		return this->BoardCargo(CargoLoad{this, dest, max_move, current_tile}, next_station, vehicle, boarding);
	}
}

/**
 * Routes packets with station "avoid" as next hop to a different place.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to append the cargo to.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 * @return The number of elements that got rerouted.
 */
uint StationCargoList::Reroute(uint max_move, StationCargoList *dest, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	return this->ShiftCargo(StationCargoReroute(this, dest, max_move, avoid, avoid2, ge), {&avoid, 1}, false);
}

/**
 * Routes packets with station "avoid" as next hop to a different place.
 * @param max_move Maximum amount of cargo to move.
 * @param dest List to append the cargo to.
 * @param avoid Station to exclude from routing and current next hop of packets to reroute.
 * @param avoid2 Additional station to exclude from routing.
 * @param ge GoodsEntry to get the routing info from.
 */
uint StationCargoList::RerouteFromSource(uint max_move, StationCargoList *dest, StationID source, StationID avoid, StationID avoid2, const GoodsEntry *ge)
{
	return this->ShiftCargoFromSource(StationCargoReroute(this, dest, max_move, avoid, avoid2, ge), source, {&avoid, 1}, false);
}

/*
 * We have to instantiate everything we want to be usable.
 */
template class CargoList<VehicleCargoList, CargoPacketList>;
template class CargoList<StationCargoList, StationCargoPacketMap>;

/**
 * Moves some cargo from one designation to another.
 * @return Amount of cargo actually reassigned.
 */
template uint VehicleCargoList::Reassign<VehicleCargoList::MoveToAction::Deliver, VehicleCargoList::MoveToAction::Keep>(uint);
