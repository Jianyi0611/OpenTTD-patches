/** @file fare_seats_sl.h Comfort levels stored on plans, order backups and fare templates. */
#ifndef FARE_SEATS_SL_H
#define FARE_SEATS_SL_H

#include "../passenger_fare.h"
#include "saveload.h"

template<class Owner, auto GetSeating>
struct FareSeatsHandler final : TypedSaveLoadStructHandler<FareSeatsHandler<Owner, GetSeating>, Owner> {
	struct Row {
		FareSeat seat;
		bool selected = false;
	};
	NamedSaveLoadTable GetDescription() const override
	{
		static const NamedSaveLoad entries[] = {
			NSLT("name", SLE_SSTR(Row, seat.name, SLE_STR)),
			NSLT("percent", SLE_VAR(Row, seat.percent, SLE_UINT16)),
			NSLT("builtin", SLE_VAR(Row, seat.builtin, SLE_UINT8)),
			NSLT("selected", SLE_VAR(Row, selected, SLE_BOOL)),
		};
		return entries;
	}
	void Save(Owner *owner) const override
	{
		const auto &seats = GetSeating(*owner);
		SlSetStructListLength(seats.levels.size());
		for (size_t i = 0; i < seats.levels.size(); ++i) {
			Row row{seats.levels[i], i == seats.selected};
			SlObjectSaveFiltered(&row, this->GetLoadDescription());
		}
	}
	void Load(Owner *owner) const override
	{
		auto &seats = GetSeating(*owner);
		seats = {};
		const size_t count = SlGetStructListLength(FareSeating::MAX_LEVELS);
		bool selected = false;
		for (size_t i = 0; i < count; ++i) {
			Row row{};
			SlObjectLoadFiltered(&row, this->GetLoadDescription());
			if (row.seat.percent > PassengerTariff::maxima[PassengerTariff::Standard] || row.seat.builtin > 3 || (row.seat.name.empty() && row.seat.builtin == 3)) SlErrorCorrupt("Invalid fare comfort level");
			if (row.selected) {
				if (selected) SlErrorCorrupt("Multiple selected fare levels");
				selected = true;
				seats.selected = static_cast<uint16_t>(i);
			}
			seats.levels.push_back(std::move(row.seat));
		}
		if (count && !selected) SlErrorCorrupt("Missing selected fare level");
	}
	void LoadCheck(Owner *owner) const override { this->Load(owner); }
};
#endif
