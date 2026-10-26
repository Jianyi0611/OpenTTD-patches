/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file saveload_common_type.h Common types for saving and loading games. */

#ifndef SL_SAVELOAD_COMMON_TYPE_H
#define SL_SAVELOAD_COMMON_TYPE_H

struct SaveLoad;
struct NamedSaveLoad;

/** A table of SaveLoad entries. */
using SaveLoadTable = std::span<const SaveLoad>;
using NamedSaveLoadTable = std::span<const NamedSaveLoad>;

namespace upstream_sl {
	struct SaveLoad;

	/** A table of SaveLoad entries. */
	using SaveLoadTable = std::span<const SaveLoad>;
}

#endif /* SL_SAVELOAD_COMMON_TYPE_H */
