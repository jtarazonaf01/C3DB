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

#include <cstddef>
#include <cstdint>

#include "c3db_db_file.h"
#include "c3db_file.h"
#include "c3db_types.h"

/**
 * Metadata DBF plus raw fixed-size data file.
 *
 * c3db_data_file_t is intended for large fixed-size payloads. The metadata
 * file (.met) is a DBF whose payload stores the rows used in the data file
 * (.dat). The data file stores the large payload directly and has no cache or
 * log.
 *
 * The .met DBF payload stores two .dat row references:
 *
 *   active + active_crc: visible payload row
 *   spare  + spare_crc : previous payload row usable as fallback
 *
 * The stored reference is the .dat row itself. UINT32_MAX is reserved as the
 * null value, so row 0 remains a valid data row and no row+1 translation is
 * needed.
 *
 * CRC fields validate a small payload sample made from the first and last
 * C3DB_DATA_CRC_SAMPLE_SIZE bytes. A CRC value of zero means the corresponding
 * .dat row is not valid. This keeps the consistency check cheap while still
 * detecting the incomplete-write cases expected on microSD.
 *
 * Updating writes the large payload into spare first, then publishes the new
 * reference through the DBF slot mechanism. If publication is interrupted, DBF
 * recovery keeps the previous metadata slot visible.
 */
class c3db_data_file_t : public c3db_db_file_t
{
public:
  //! Creates a data-file wrapper for large fixed-size payloads.
  explicit c3db_data_file_t(size_t data_size);

  //! Closes both .met and .dat.
  ~c3db_data_file_t() override;

  //! Creates "<base>.met" and "<base>.dat".
  c3db_err_t create(const char* base_file_name);

  //! Opens "<base>.met" and "<base>.dat".
  c3db_err_t begin(
    const char* base_file_name,
    bool read_only = false
  );

  //! Closes both files.
  c3db_err_t end() override;

  //! Appends a new large payload and publishes its .dat row in .met.
  c3db_err_t append(const uint8_t* data, c3db_id_t &id) override;

  //! Inserts a large payload, reusing .met and associated .dat rows when possible.
  c3db_err_t insert(const uint8_t* data, c3db_id_t &id) override;

  //! Reads a large payload through its .met reference.
  c3db_err_t select(c3db_id_t id, uint8_t* data) override;

  //! Writes a new large payload and switches the .met pointer to the other slot.
  c3db_err_t update(c3db_id_t id, const uint8_t* data) override;

  //! Deletes only the .met record; .dat rows remain available for reuse.
  c3db_err_t remove(c3db_id_t id) override;

  //! Imports large fixed-size payloads from an already opened file.
  c3db_err_t import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added) override;

  //! Imports large fixed-size payloads from a physical file.
  c3db_err_t import_file(const char* source_file_name, c3db_id_t &first_id, size_t &rows_added) override;

  //! Exports active payloads to an already opened file.
  c3db_err_t export_file(FILE* target_file, size_t &rows_exported) override;

  //! Exports active payloads to a new physical file.
  c3db_err_t export_file(const char* target_file_name, size_t &rows_exported) override;

  //! Rebuilds .met and .dat so active payloads occupy a compact .dat file.
  //!
  //! External ids remain valid because .met rows are not compacted: the
  //! temporary metadata copy is rebuilt with the same physical row ids and
  //! cycles, but its active payloads point to compacted rows in the temporary
  //! data file. Publication uses backup files so begin() can recover the
  //! previous pair after an interrupted replacement.
  c3db_err_t defrag();

  //! Returns the fixed large-payload size stored in .dat.
  size_t payload_size() const;

private:
  //! Sentinel used when a .met slot has no associated .dat row yet.
  static constexpr uint32_t C3DB_DATA_ROW_NULL = UINT32_MAX;

  struct c3db_data_ref_t
  {
    uint32_t active = C3DB_DATA_ROW_NULL;
    uint32_t active_crc = 0;
    uint32_t spare = C3DB_DATA_ROW_NULL;
    uint32_t spare_crc = 0;
  };

  //! Bytes sampled at both payload edges for .dat consistency validation.
  static constexpr size_t C3DB_DATA_CRC_SAMPLE_SIZE = 8;

  //! Releases the stored base file name.
  void release_base_file_name();

  //! Removes a physical file only when it exists.
  c3db_err_t delete_if_exists(const char* file_name) const;

  //! Restores a safe pair after an interrupted defrag, when possible.
  c3db_err_t recover_defrag_files(const char* base_file_name) const;

  //! Publishes metadata records for rows already appended to .dat.
  c3db_err_t publish_imported_rows(size_t first_dat_row, size_t rows_added, c3db_id_t &first_id);

  //! Exports active rows to an opened c3db_file_t target.
  c3db_err_t export_to(c3db_file_t &target_file, size_t &rows_exported);

  //! Copies one .dat row to the export target in shared-buffer chunks.
  c3db_err_t export_data_row(uint32_t row_ref, c3db_file_t &target_file);

  //! Calculates the sampled CRC stored in .met for one payload.
  uint32_t payload_crc(const uint8_t* data) const;

  //! Calculates the sampled CRC for a payload already stored in .dat.
  c3db_err_t payload_crc(uint32_t row_ref, uint32_t &crc);

  //! Writes a payload to an existing .dat row or appends a new one.
  c3db_err_t write_data_row(uint32_t &row_ref, const uint8_t* data);

  //! Reads active payload, falling back to spare when active CRC validation fails.
  c3db_err_t read_data_row(const c3db_data_ref_t &ref, uint8_t* data);

  //! Returns the valid .dat row referenced by active or spare.
  c3db_err_t get_row_ref(const c3db_data_ref_t &ref, uint32_t &row_ref, uint32_t &crc);

  size_t payload_size_;
  char* base_file_name_;
  c3db_file_t dat_file_;
};
