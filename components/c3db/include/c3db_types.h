/*
 * C3DB - Embedded Persistent Database for IoT Systems
 *
 * Copyright (c) 2026 Jose Tarazona Franco
 *
 * Licensed under the Apache License, Version 2.0.
 * You may obtain a copy of the License at:
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.
 *
 * This file is part of the C3DB project developed as part of a
 * Bachelor's Thesis (TFG).
 */

#pragma once

#include <cstdint>

//! Public logical record identifier used by C3DB database files.
using c3db_id_t = uint64_t;

//! Null logical record identifier.
static constexpr c3db_id_t C3DB_NULL_ID = UINT64_MAX;

/**
 * Error codes returned by C3DB operations.
 */
enum c3db_err_t : int
{
  C3DB_FREE_LIST_REPAIR_WRN = -1, ///< Free-list repair is required; DBF remains usable with a fresh free-list
  C3DB_ORPHANED_SPACE_WRN = -2,   ///< Operation completed, but obsolete storage could not be released

  C3DB_OK = 0,                  ///< Operation completed successfully
  C3DB_GENERIC_ERR = 1,         ///< Generic error
  C3DB_INVALID_ARG_ERR,         ///< Invalid argument
  C3DB_UNSUPPORTED_OP_ERR,      ///< Operation is not supported by this class
  C3DB_NO_RECORDS_ERR,          ///< Record operation requested on a header-only file
  C3DB_READ_ONLY_ERR,           ///< Write operation requested on a read-only file
  C3DB_REC_CORRUPT_ERR,         ///< Record failed integrity validation
  C3DB_REC_NOT_FOUND_ERR,       ///< Logical record was not found

  C3DB_FILE_SIZE_ERR,           ///< File size is incompatible with the configured layout
  C3DB_FILE_BAD_NAME_ERR,       ///< File name is null or empty
  C3DB_FILE_OPEN_ERR,           ///< File open operation failed
  C3DB_FILE_NOT_OPEN_ERR,       ///< File operation requested before opening a file
  C3DB_FILE_ALREADY_OPEN_ERR,   ///< File is already open
  C3DB_FILE_ALREADY_EXISTS_ERR, ///< File already exists
  C3DB_FILE_CORRUPT_ERR,        ///< File layout is corrupted
  C3DB_FILE_READ_ERR,           ///< File read operation failed
  C3DB_EOF_ERR,                 ///< End of file reached
  C3DB_FILE_SEEK_ERR,           ///< File seek operation failed
  C3DB_FILE_TELL_ERR,           ///< File position query failed
  C3DB_FILE_FLUSH_ERR,          ///< File flush operation failed
  C3DB_FILE_WRITE_ERR,          ///< File write operation failed
  C3DB_FILE_CLOSE_ERR,          ///< File close operation failed
  C3DB_FILE_DELETE_ERR,         ///< File delete operation failed
  C3DB_FILE_RENAME_ERR,         ///< File rename operation failed
  C3DB_FREE_LIST_CORRUPT_ERR,   ///< Free-list metadata is corrupt; repair_free_list() is required
  C3DB_FREE_LIST_EMPTY_ERR,     ///< Free-list pop requested when the list has no available records
  C3DB_COUNT_UNKNOWN_ERR        ///< Active record count cannot be calculated from current metadata
};

/**
 * Logical cache memory distribution strategy.
 */
enum c3db_cache_mode_t : uint8_t
{
  C3DB_BALANCED_MODE,          ///< Balanced cache distribution
  C3DB_SEQUENTIAL_ACCESS_MODE, ///< Optimized for contiguous reads
  C3DB_BULK_INSERT_MODE        ///< Optimized for sequential appends
};
