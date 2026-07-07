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
#include <stdio.h>

#include "c3db_file.h"
#include "c3db_types.h"

struct c3db_header_t;
struct c3db_cached_rec_t;

/**
 * Common validated-record view.
 *
 * c3db_rec_t owns no storage. It wraps a caller-provided byte buffer and
 * exposes the logical state shared by DBF records and compact cached records:
 * active, deleted or corrupt, plus the physical data slot that contains the
 * last valid payload metadata. Derived classes decide whether those values are
 * stored as object attributes or inside the wrapped bytes.
 *
 * DELETE_BLOCK stores free-list linkage for deleted records. DATA_BLOCK stores
 * the sequence, generation cycle and CRC for one payload. Derived classes
 * define the concrete byte layout and expose the current DATA_BLOCK through
 * data_block().
 */
struct c3db_rec_t
{
  //! Physical data slot identifiers used by DBF records.
  static constexpr uint8_t C3DB_REC_SLOT_0 = 0;
  static constexpr uint8_t C3DB_REC_SLOT_1 = 1;
  //! No valid data slot is currently known.
  static constexpr uint8_t C3DB_REC_NO_SLOT = 3;
  
  //! Logical record states after validation.
  static constexpr uint8_t C3DB_REC_ACTIVE = 0;
  static constexpr uint8_t C3DB_REC_DELETED = 1;
  static constexpr uint8_t C3DB_REC_CORRUPT = 2;
  
  //! Deleted/free-list metadata persisted at the beginning of each DBF record.
  struct __attribute__((packed)) del_block_t
  {
    uint32_t crc = 0;
    uint32_t seq = 0;
    uint32_t next_free = UINT32_MAX;
  };

  //! Data metadata stored before each logical payload.
  struct __attribute__((packed)) data_block_t
  {
    uint32_t crc = 0;
    uint32_t seq = 0;
    uint32_t cycle = 0;
  };

protected:
  //! Logical payload size owned by the record view.
  size_t data_size_;
  //! Physical record buffer owned by the caller.
  uint8_t* raw_data_;

public:
  c3db_rec_t(uint8_t* raw_data, size_t data_size);
  virtual ~c3db_rec_t() = default;

  c3db_rec_t(const c3db_rec_t&) = delete;
  c3db_rec_t& operator=(const c3db_rec_t&) = delete;

  //! Returns the wrapped raw record buffer.
  uint8_t* bytes();
  //! Returns the wrapped raw record buffer.
  const uint8_t* bytes() const;
  //! Returns the parsed logical state.
  virtual uint8_t state() const = 0;
  //! Returns true when the parsed state is active or deleted.
  bool is_valid() const;
  //! Returns true when the record currently exposes a logical payload.
  bool is_active() const;
  //! Returns true when the record is linked or linkable through DELETE_BLOCK.
  bool is_deleted() const;

  //! Returns the next physical row in the free-list stored by DELETE_BLOCK.
  uint32_t next_free() const;
  //! Returns the current generation cycle for active records, or UINT32_MAX otherwise.
  uint32_t cycle() const;
  //! Returns the byte size of the concrete record representation.
  virtual size_t rec_size() const = 0;

  //! Initializes an empty record as active with sequence 1 and cycle 0.
  c3db_err_t initialize(const uint8_t* payload);
  //! Writes a new payload and persists the corresponding DATA_BLOCK.
  virtual c3db_err_t save_payload(c3db_file_t* file, size_t row_id, const uint8_t* payload) = 0;
  //! Writes DELETE_BLOCK and persists the deleted/free-list state.
  c3db_err_t save_deleted(c3db_file_t* file, size_t row_id, uint32_t next_free);
  //! Copies the current logical payload into buffer.
  //!
  //! The caller controls whether reading deleted records is allowed.
  virtual c3db_err_t get_payload(uint8_t* buffer) const = 0;
  //! Writes the active logical payload to another file.
  virtual c3db_err_t export_payload(c3db_file_t* target_file) const = 0;
protected:
  //! Calculates CRC for a DELETE_BLOCK value.
  static uint32_t del_block_crc(const del_block_t &state);
  //! Validates a DELETE_BLOCK value.
  static bool del_block_is_valid(const del_block_t &state);
  //! Calculates CRC for DATA_BLOCK metadata plus payload.
  static uint32_t data_block_crc(uint32_t seq, uint32_t cycle, const uint8_t* payload, size_t data_size);
  //! Validates DATA_BLOCK metadata plus payload.
  static bool data_block_is_valid(const data_block_t &slot, const uint8_t* payload, size_t data_size);
  
  //! Compares modular uint32_t sequence values.
  static bool seq_newer(uint32_t a, uint32_t b);

  //! Returns DELETE_BLOCK in the wrapped buffer.
  virtual del_block_t* del_block() = 0;
  //! Returns DELETE_BLOCK in the wrapped buffer.
  virtual const del_block_t* del_block() const = 0;
  //! Returns the last valid DATA_BLOCK according to data_slot(), or nullptr.
  virtual data_block_t* data_block() = 0;
  //! Returns the last valid DATA_BLOCK according to data_slot(), or nullptr.
  virtual const data_block_t* data_block() const = 0;

  //! Returns the latest cycle from DATA_BLOCK for active or deleted records.
  uint32_t get_last_cycle() const;
  //! Returns the next generation cycle, wrapping UINT32_MAX - 1 to 0.
  uint32_t get_next_cycle() const;
  //! Returns the next sequence value for the record.
  c3db_err_t get_next_seq(uint32_t &seq) const;
  //! Returns the newest sequence among DELETE_BLOCK and current DATA_BLOCK.
  uint32_t get_last_seq() const;

  //! Resets parsed state to corrupt/no-slot.
  void reset_state();

  //! Returns the opposite physical data slot.
  uint8_t next_data_slot() const;

  //! Returns the physical slot that stores the latest valid data block.
  virtual uint8_t data_slot() const = 0;
  //! Sets the parsed logical state.
  virtual void set_state(uint8_t state) = 0;
  //! Sets the physical slot that stores the latest valid data block.
  virtual void set_data_slot(uint8_t data_slot) = 0;

  //! Writes DELETE_BLOCK with the supplied next free row.
  //!
  //! The caller must ensure the state transition is valid for the operation.
  c3db_err_t set_deleted(uint32_t next_free);

  //! Writes a new payload while preserving cycle for updates and advancing it when reactivating deleted rows.
  //!
  //! The caller must ensure the state transition is valid for the operation.
  c3db_err_t set_payload(const uint8_t* payload);
  //! Concrete payload writer used by set_payload().
  virtual c3db_err_t set_payload(uint32_t seq, uint32_t cycle, const uint8_t* payload) = 0;    

  //! Persists only DELETE_BLOCK for this record row.
  c3db_err_t write_deleted(c3db_file_t* file, size_t row_id);
  //! Persists only the current DATA_BLOCK plus payload for this record row.
  virtual c3db_err_t write_payload(c3db_file_t* file, size_t row_id) = 0;
  
};

/**
 * DBF physical record with dual data slots and one deleted/free state.
 *
 * Each record is split into independently validated groups:
 *
 *   DELETE_BLOCK | DATA_BLOCK[0] | DATA_BLOCK[1]
 *
 *   DELETE_BLOCK = crc | seq | next_free
 *   DATA_BLOCK  = crc | seq | cycle | payload
 *
 * A group with seq == 0 is considered unused. Otherwise its CRC must match the
 * remaining fields in that group. A record is active when the newest valid
 * data slot is newer than DELETE_BLOCK; it is deleted when DELETE_BLOCK is valid
 * and newer than the newest valid data slot.
 *
 * This makes recovery local: an interrupted write only invalidates the group
 * being written. The previous valid group remains available without a redo log.
 */
struct c3db_db_rec_t : public c3db_rec_t
{
  /*
   * raw_data_ layout:
   *
   *   DELETE_BLOCK | DATA_BLOCK[0] + payload | DATA_BLOCK[1] + payload
   *
   * DELETE_BLOCK starts at offset 0. Each DATA_BLOCK group stores its header
   * followed immediately by one logical payload of data_size_ bytes.
   */
public:
  //! Creates a record view over an external physical buffer.
  c3db_db_rec_t(uint8_t* raw_data, size_t data_size);
  using c3db_rec_t::initialize;
  using c3db_rec_t::set_payload;

  c3db_err_t read_rec(c3db_file_t* file, size_t row_id);
  c3db_err_t write_rec(c3db_file_t* file, size_t row_id);
  //! Appends the complete DBF physical record representation to file.
  c3db_err_t add_rec(c3db_file_t* file, size_t &row_id);

  uint8_t state() const override;
  size_t rec_size() const override;
  static size_t rec_size(size_t data_size);
  c3db_err_t parse();

  c3db_err_t save_payload(c3db_file_t* file, size_t row_id, const uint8_t* payload) override;
  c3db_err_t get_payload(uint8_t* buffer) const override;
  c3db_err_t export_payload(c3db_file_t* target_file) const override;

  //! Exports the current validated DBF state to a compact cached record.
  c3db_err_t export_to(c3db_cached_rec_t &cached_rec) const;

protected:
  //! Parsed logical record state.
  uint8_t state_;
  //! Physical slot that stores the latest valid data block.
  uint8_t data_slot_;

  del_block_t* del_block() override;
  const del_block_t* del_block() const override;
  data_block_t* data_block() override;
  const data_block_t* data_block() const override;
  uint8_t data_slot() const override;
  void set_state(uint8_t state) override;
  void set_data_slot(uint8_t data_slot) override;
  c3db_err_t write_payload(c3db_file_t* file, size_t row_id) override;
  c3db_err_t set_payload(uint32_t seq, uint32_t cycle, const uint8_t* payload) override;

  static constexpr uint32_t C3DB_INITIAL_CYCLE = 0;

private:
  size_t data_block_size() const;
  size_t slot_offset(uint8_t slot) const;

  const uint8_t* payload(uint8_t slot) const;
  data_block_t* data_block(uint8_t slot);
  const data_block_t* data_block(uint8_t slot) const;
  void set_payload(uint8_t slot, uint32_t seq, uint32_t cycle, const uint8_t* payload);

  bool newest_valid_slot(uint8_t &slot) const;
};

/**
 * Fixed-size payload database file with per-record slot consistency.
 *
 * The DBF layer stores user payloads in fixed-size logical records identified
 * by c3db_id_t. The identifier packs the physical row and a generation cycle,
 * so a deleted row reused later does not validate old identifiers.
 *
 * Physical file layout:
 *
 *   header | record[0] | record[1] | ...
 *
 * The header keeps a repair flag and two validated copies of the global
 * free-list state:
 *
 *   magic | version | repair_needed | ctrl_free[2]
 *   ctrl_free = crc | seq | first | count
 *
 * first_free and next_free store physical row ids. UINT32_MAX means null, so
 * row 0 is a valid reusable row. repair_needed marks that deleted slots may
 * have been lost from the free-list and a full repair pass should be run.
 */
class c3db_db_file_t
{
public:
  //! Creates a DBF wrapper for fixed-size payloads.
  explicit c3db_db_file_t(size_t data_size);

  //! Releases the DBF wrapper and its internal work buffer.
  virtual ~c3db_db_file_t();

  //! Creates and opens a new DBF file using ".dbf" by default.
  virtual c3db_err_t create(const char* base_file_name, const char* extension = ".dbf");

  //! Opens an existing DBF file using ".dbf" by default.
  virtual c3db_err_t begin(const char* base_file_name, bool read_only = false, const char* extension = ".dbf");

  /**
   * Uses an already opened DBF file.
   *
   * The caller keeps ownership of the FILE pointer. Access must be exclusive
   * while this DBF wrapper is active.
   */
  virtual c3db_err_t begin(FILE* file, bool read_only = false);

  //! Closes the DBF file and releases transient memory.
  virtual c3db_err_t end();

  //! Appends a new active record at the physical end of the file.
  //!
  //! The initial record has DATA_BLOCK[0] valid; DELETE_BLOCK and DATA_BLOCK[1] are unused.
  virtual c3db_err_t append(const uint8_t* data, c3db_id_t &id);

  //! Inserts a record, reusing a deleted slot when possible.
  //!
  //! When reusing a row, the row is removed from the global free-list before it
  //! is reactivated. A reset may orphan the slot, but cannot leave the free-list
  //! pointing to a row that is active again. If corrupted free-list metadata is
  //! found, the list is reset, repair_needed is persisted and the record is
  //! appended at the physical end while returning C3DB_FREE_LIST_REPAIR_WRN.
  virtual c3db_err_t insert(const uint8_t* data, c3db_id_t &id);

  //! Imports fixed-size payloads from an already opened file.
  //!
  //! The source file must contain raw logical payloads laid out consecutively.
  //! A final incomplete payload is ignored, matching the DBF recovery policy
  //! that only complete records are visible.
  virtual c3db_err_t import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added);

  //! Imports fixed-size payloads from a physical file.
  virtual c3db_err_t import_file(const char* source_file_name, c3db_id_t &first_id, size_t &rows_added);

  //! Exports active payloads to an already opened file.
  virtual c3db_err_t export_file(FILE* target_file, size_t &rows_exported);

  //! Exports active payloads to a new physical file.
  virtual c3db_err_t export_file(const char* target_file_name, size_t &rows_exported);

  //! Reads an active payload by logical identifier.
  virtual c3db_err_t select(c3db_id_t id, uint8_t* data);

  //! Updates an active payload by writing the inactive data slot.
  //!
  //! The data slot contains both cycle and payload under the same CRC. If the
  //! write is interrupted, the previous valid slot remains visible.
  virtual c3db_err_t update(c3db_id_t id, const uint8_t* data);

  //! Marks an active record as deleted and links it into the free-list.
  //!
  //! DELETE_BLOCK is written before the global header is updated. A reset between
  //! both writes can orphan the deleted row, but it cannot expose a wrong active
  //! record.
  virtual c3db_err_t remove(c3db_id_t id);

  //! Returns the configured payload size.
  size_t data_size() const;

  //! Returns an upper bound for active records.
  //!
  //! The value is exact while the free-list contains every deleted row. If
  //! reset leaves deleted rows orphaned, the result may overestimate active
  //! records until repair_free_list() rebuilds the free-list.
  virtual c3db_err_t max_rec_count(size_t &count) const;

  //! Returns true when a full free-list repair is pending.
  bool free_list_repair_needed() const;

  //! Rebuilds the free-list metadata by scanning all physical records.
  //!
  //! A successful repair also clears the persistent repair_needed flag.
  c3db_err_t repair_free_list(const char* base_file_name, const char* extension = ".dbf");

  /**
   * Rebuilds free-list metadata in an already opened DBF file.
   *
   * A successful repair also clears the persistent repair_needed flag.
   *
   * The caller keeps ownership of the FILE pointer. Access must be exclusive
   * while the repair is running.
   */
  c3db_err_t repair_free_list(FILE* file);

  //! Returns true when the underlying file is open.
  bool is_open() const;

  //! Returns true when the underlying file is read-only.
  bool is_read_only() const;

  //! Gives controlled low-level access to the underlying physical file.
  c3db_file_t& file(); 
  const c3db_file_t& file() const;

protected:
  //! Returns the physical DBF record size.
  size_t rec_size() const;

  //! Loads row_id into the internal cached record unless already cached.
  c3db_err_t read_record(size_t row_id);
  //! Loads row_id into the supplied record view.
  c3db_err_t read_record(size_t row_id, c3db_db_rec_t &rec);
  //! Appends a new active DBF record and returns its logical id.
  c3db_err_t append_data(const uint8_t* data, c3db_id_t &id);
  //! Pushes a deleted physical row into the persistent free-list.
  c3db_err_t add_free(uint32_t row_id);
  //! Pops the persistent free-list head after a row has been selected for reuse.
  c3db_err_t pop_free(uint32_t next_first);
  //! Persists the free-list repair flag and resets unsafe free-list metadata when needed.
  c3db_err_t set_free_list_repair_needed(bool value);
  //! Exports active logical payloads to target_file.
  c3db_err_t export_to(c3db_file_t &target_file, size_t &rows_exported);
  //! Allocates transient record storage and loads DBF header metadata.
  c3db_err_t init_dbf();
  //! Loads or initializes the DBF header controller.
  c3db_err_t load_header();
  //! Rebuilds free-list metadata on an already opened file.
  c3db_err_t repair_free_list();
  //! Clears transient DBF state after close or failed open.
  void reset_state();

  //! Null physical row reference used by the DBF free-list.
  static constexpr uint32_t C3DB_NULL_REF = UINT32_MAX;

  //! Logical payload size stored by each DBF record.
  size_t data_size_;
  //! Underlying fixed-record file.
  c3db_file_t file_;
  //! Persistent DBF header controller.
  c3db_header_t* header_;
  //! First reusable physical row.
  uint32_t first_free_;
  //! Number of rows currently known in the free-list.
  uint32_t free_count_;
  //! True when deleted rows may be missing from the free-list.
  bool free_list_repair_needed_;
  //! Raw storage used by rec_.
  uint8_t* rec_buf_;
  //! Cached view over rec_buf_ for the last loaded physical row.
  c3db_db_rec_t* rec_;
  //! Physical row currently loaded in rec_, or C3DB_NULL_REF when invalid.
  uint32_t cached_row_;
};
