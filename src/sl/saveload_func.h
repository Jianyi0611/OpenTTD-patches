/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file saveload_func.h Functions related to saving and loading games. */

#ifndef SL_SAVELOAD_FUNC_H
#define SL_SAVELOAD_FUNC_H

#include "../fileio_type.h"
#include "../fios.h"
#include "../strings_id_type.h"

/** Save or load result codes. */
enum class SaveLoadResult : uint8_t {
	Ok, ///< completed successfully
	Error, ///< error that was caught before internal structures were modified
	ReInit, ///< error that was caught in the middle of updating game state, need to clear it. (can only happen during load)
};

/** Deals with the type of the savegame, independent of extension */
struct FileToSaveLoad {
	SaveLoadOperation file_op;       ///< File operation to perform.
	FiosType ftype;                  ///< File type.
	std::string name;                ///< Name of the file.
	EncodedString title;             ///< Internal name of the game.

	void SetMode(const FiosType &ft, SaveLoadOperation fop = SaveLoadOperation::Load);
	void Set(const FiosItem &item);
};

enum SaveModeFlags : uint8_t {
	SMF_NONE             = 0,
	SMF_NET_SERVER       = 1 << 0, ///< Network server save
	SMF_ZSTD_OK          = 1 << 1, ///< Zstd OK
	SMF_SCENARIO         = 1 << 2, ///< Scenario save
};
DECLARE_ENUM_AS_BIT_SET(SaveModeFlags);

extern FileToSaveLoad _file_to_saveload;

std::string GenerateDefaultSaveName();
EncodedString GetSaveLoadErrorType();
EncodedString GetSaveLoadErrorMessage();
SaveLoadResult SaveOrLoad(const std::string &filename, SaveLoadOperation fop, DetailedFileType dft, Subdirectory sb, bool threaded = true, SaveModeFlags flags = SMF_NONE);
void WaitTillSaved();
void ProcessAsyncSaveFinish();
void DoExitSave();

void DoAutoOrNetsave(FiosNumberedSaveName &counter, bool threaded, FiosNumberedSaveName *lt_counter = nullptr);

SaveLoadResult SaveWithFilter(std::shared_ptr<struct SaveFilter> writer, bool threaded, SaveModeFlags flags);
SaveLoadResult LoadWithFilter(std::shared_ptr<struct LoadFilter> reader);

extern bool _do_autosave;

#endif /* SL_SAVELOAD_FUNC_H */
