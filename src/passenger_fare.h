/** @file passenger_fare.h Passenger tariffs. Stored values are one plus their displayed value; zero inherits. */
#ifndef PASSENGER_FARE_H
#define PASSENGER_FARE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct PassengerTariff {
	enum Field : uint8_t { ShortEnd, MediumEnd, ShortPrice, MediumPrice, LongPrice, Economy, Standard, Comfort, SlowEnd, NormalEnd, FastEnd, Slow, Normal, Fast, Express, Class, Fields };
	std::array<uint16_t, Fields> values{};
	uint16_t template_id = 0; ///< Company template selected by this vehicle, one-based. Zero uses its usual tariff.
	static constexpr std::array<uint16_t, Fields> defaults{50, 200, 12, 10, 8, 90, 100, 115, 80, 160, 250, 95, 100, 110, 120, 1};
	static constexpr std::array<uint16_t, Fields> maxima{4095, 4096, 1000, 1000, 1000, 500, 500, 500, 1000, 1000, 1000, 500, 500, 500, 500, 2};
	bool Active() const { return *this != PassengerTariff{}; }
	bool operator==(const PassengerTariff &) const = default;
	uint Get(Field f, const PassengerTariff &company, const PassengerTariff *schedule = nullptr) const
	{
		if (values[f]) return values[f] - 1;
		if (schedule && schedule->values[f]) return schedule->values[f] - 1;
		return company.values[f] ? company.values[f] - 1 : defaults[f];
	}

	/** Price in hundredths of the base currency, before allocating this ride's share of the journey. */
	uint64_t Price(uint distance, uint speed, const PassengerTariff &company, const PassengerTariff *schedule = nullptr, std::optional<uint> seat_percent = {}) const
	{
		const uint short_end = Get(ShortEnd, company, schedule);
		const uint medium_end = std::max(short_end, Get(MediumEnd, company, schedule));
		const uint short_leg = std::min(distance, short_end);
		const uint medium_leg = std::min(distance, medium_end) - short_leg;
		const uint64_t base = uint64_t(short_leg) * Get(ShortPrice, company, schedule) + uint64_t(medium_leg) * Get(MediumPrice, company, schedule) +
				uint64_t(distance - short_leg - medium_leg) * Get(LongPrice, company, schedule);
		const auto seat = static_cast<Field>(Economy + std::min(2U, Get(Class, company, schedule)));
		const auto band = speed <= Get(SlowEnd, company, schedule) ? Slow : speed <= Get(NormalEnd, company, schedule) ? Normal : speed <= Get(FastEnd, company, schedule) ? Fast : Express;
		return (base * seat_percent.value_or(Get(seat, company, schedule)) * (speed == 0 ? 100U : Get(band, company, schedule)) + 500) / 1000;
	}
};

struct FareSeat {
	std::string name;
	uint16_t percent = 100;
	uint8_t builtin = 3; ///< 0..2 retain the corresponding native seat and its per-train override.
	std::string Key() const { return name.empty() ? std::to_string(builtin) : "+" + name; }
	bool operator==(const FareSeat &) const = default;
};

struct FareSeating {
	static constexpr size_t MAX_LEVELS = 256;
	static constexpr uint NAME_CHARS = 48;
	std::vector<FareSeat> levels; ///< Empty uses the original three inherited rates.
	uint16_t selected = 0;
	bool operator==(const FareSeating &) const = default;
};

enum FareSeatAction : uint8_t { FSA_ADD, FSA_RENAME, FSA_REMOVE, FSA_RATE, FSA_SELECT, FTA_CREATE, FTA_RENAME, FTA_FIELD };

struct ScheduleTariff : PassengerTariff {
	FareSeating seating;
	bool Active() const { return !seating.levels.empty() || PassengerTariff::Active(); }
	FareSeating EditableSeating(const PassengerTariff &company) const
	{
		if (!seating.levels.empty()) return seating;
		FareSeating result;
		result.selected = Get(Class, company);
		for (uint8_t i = 0; i < 3; ++i) result.levels.push_back({{}, static_cast<uint16_t>(Get(static_cast<Field>(Economy + i), company)), i});
		return result;
	}
	uint64_t Quote(uint distance, uint speed, const PassengerTariff &company, const PassengerTariff &train = {}) const
	{
		std::optional<uint> percent;
		if (!train.values[Class] && seating.selected < seating.levels.size()) {
			const auto &seat = seating.levels[seating.selected];
			if (seat.builtin < 3 && train.values[Economy + seat.builtin]) {
				percent = train.values[Economy + seat.builtin] - 1;
			} else {
				percent = seat.percent;
			}
		}
		return train.Price(distance, speed, company, this, percent);
	}
};

#endif
