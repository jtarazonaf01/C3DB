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
#include "c3db_types.h"

/**
 * Compact cached record view with one active payload.
 *
 * This type will back the cached DBF fast path. It derives from c3db_rec_t so
 * both persistent and cached record views share the same basic buffer ownership
 * model.
 */
struct c3db_cached_rec_t : public c3db_rec_t
{
  /*
   * raw_data_ layout:
   *
   *   REC_STATE | DATA_SLOT | DELETE_BLOCK | DATA_BLOCK + payload
   *
   * Unlike c3db_db_rec_t, the cached representation stores its logical state
   * and one compact DATA_BLOCK inside the cache bytes.
   */
  c3db_cached_rec_t(uint8_t* raw_data, size_t data_size);
  using c3db_rec_t::initialize;
  using c3db_rec_t::set_payload;

  size_t rec_size() const override;
  static size_t rec_size(size_t data_size);
  c3db_err_t initialize(
    uint8_t state,
    uint8_t data_slot,
    const del_block_t &delete_block,
    const data_block_t &data_block,
    const uint8_t* payload
  );
  c3db_err_t save_payload(c3db_file_t* file, size_t row_id, const uint8_t* payload) override;
  c3db_err_t get_payload(uint8_t* buffer) const override;
  c3db_err_t export_payload(c3db_file_t* target_file) const override;
  //! Exports the compact cached state to an initial DBF physical record.
  c3db_err_t export_to(c3db_db_rec_t &db_rec) const;

protected:
  uint8_t state() const override;
  del_block_t* del_block() override;
  const del_block_t* del_block() const override;
  data_block_t* data_block() override;
  const data_block_t* data_block() const override;
  uint8_t data_slot() const override;
  void set_state(uint8_t state) override;
  void set_data_slot(uint8_t data_slot) override;
  c3db_err_t write_payload(c3db_file_t* file, size_t row_id) override;
  c3db_err_t set_payload(uint32_t seq, uint32_t cycle, const uint8_t* payload) override;

private:
  static constexpr size_t STATE_OFFSET = 0;
  static constexpr size_t DATA_SLOT_OFFSET = STATE_OFFSET + sizeof(uint8_t);
  static constexpr size_t DEL_BLOCK_OFFSET = DATA_SLOT_OFFSET + sizeof(uint8_t);
  static constexpr size_t DATA_BLOCK_OFFSET = DEL_BLOCK_OFFSET + sizeof(del_block_t);
  static constexpr size_t PAYLOAD_OFFSET = DATA_BLOCK_OFFSET + sizeof(data_block_t);

  const uint8_t* payload() const;
};

/**
 * Cached DBF wrapper with sequential read, historical read and write caches.
 *
 * The cache stores logical payloads plus the physical state needed to continue
 * DBF updates without re-reading the same record.
 *
 * Cache lookup policy:
 *
 * - write_cache_ stores only records appended in memory and not yet persisted.
 *   Those rows cannot exist in seq_cache_ or hist_cache_.
 * - seq_cache_ is the primary persisted-row cache. It is addressed directly by
 *   physical row id, so lookup is O(1) while the row is inside the sequential
 *   window.
 * - hist_cache_ is secondary. It may contain older copies of records that were
 *   moved out of seq_cache_. When duplicates exist, readers must prefer the
 *   newest historical entry.
 *
 * select() checks write_cache_, then seq_cache_, then hist_cache_, and reloads
 * seq_cache_ on miss. update() and remove() use seq_cache_ when the target row
 * is in the sequential window. If the row is not in seq_cache_, they may use
 * hist_cache_ to avoid a disk read. On cache miss, update() and remove()
 * delegate to c3db_db_file_t without reloading seq_cache_.
 *
 * memory_size is rounded down to a whole number of cache entries. At least
 * three entries are required: one for sequential reads, one for historical
 * reads and one for pending writes.
 *
 * By default, append() buffers new records in write_cache_ and returns their
 * future logical ids before they are physically persisted. commit() or end()
 * publishes those records. Set autocommit to true when every append must be
 * committed immediately.
 */
class c3db_cached_db_file_t : public c3db_db_file_t
{
public:
  using c3db_db_file_t::begin;

  //! Creates a cached DBF wrapper for fixed-size logical payloads and cache memory.
  c3db_cached_db_file_t(size_t data_size, size_t memory_size);

  //! When true, append() commits the write cache before returning success.
  bool autocommit;

  //! Commits pending writes and releases cache memory.
  ~c3db_cached_db_file_t() override;

  //! Creates a new cached DBF file using the requested initial cache mode.
  c3db_err_t create(
    const char* base_file_name,
    c3db_cache_mode_t mode = C3DB_BALANCED_MODE,
    const char* extension = ".dbf"
  );

  //! Opens an existing cached DBF file using the requested initial cache mode.
  c3db_err_t begin(
    const char* base_file_name,
    bool read_only = false,
    c3db_cache_mode_t mode = C3DB_BALANCED_MODE,
    const char* extension = ".dbf"
  );

  //! Opens an already opened DBF stream using the requested initial cache mode.
  c3db_err_t begin(
    FILE* file,
    bool read_only = false,
    c3db_cache_mode_t mode = C3DB_BALANCED_MODE
  );

  //! Commits pending writes, closes the DBF file and releases cache memory.
  c3db_err_t end() override;

  //! Persists pending write-cache appends to disk as DBF physical records.
  c3db_err_t commit();

  //! Returns the current cache distribution mode.
  c3db_cache_mode_t mode() const;

  //! Changes cache distribution mode after committing pending writes.
  c3db_err_t mode(c3db_cache_mode_t new_mode);

  //! Commits pending appends before importing records directly to disk.
  c3db_err_t import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added) override;

  //! Commits pending appends before importing records directly to disk.
  c3db_err_t import_file(const char* source_file_name, c3db_id_t &first_id, size_t &rows_added) override;

  //! Commits pending appends before exporting active payloads.
  c3db_err_t export_file(FILE* target_file, size_t &rows_exported) override;

  //! Commits pending appends before exporting active payloads.
  c3db_err_t export_file(const char* target_file_name, size_t &rows_exported) override;

  //! Appends a logical record to the pending write cache.
  //!
  //! With autocommit disabled, the returned id may refer to a record that is
  //! still only in RAM until commit() or end(). With autocommit enabled,
  //! append() returns success only after commit() succeeds.
  c3db_err_t append(const uint8_t* data, c3db_id_t &id) override;

  //! Inserts a logical record, committing pending appends before free-list reuse.
  c3db_err_t insert(const uint8_t* data, c3db_id_t &id) override;

  //! Reads a logical record using write, sequential and historical caches.
  c3db_err_t select(c3db_id_t id, uint8_t* data) override;

  //! Updates an active logical record and any coherent cached copies.
  c3db_err_t update(c3db_id_t id, const uint8_t* payload) override;

  //! Removes an active logical record and hides coherent cached copies.
  c3db_err_t remove(c3db_id_t id) override;

  //! Returns max DBF records plus pending write-cache appends.
  c3db_err_t max_rec_count(size_t &count) const override;

protected:
  //! CACHE_META state. DELETED and CORRUPT are negative hits, not misses.
  enum cache_entry_state_t : uint8_t
  {
    C3DB_CACHE_EMPTY = 0,
    C3DB_CACHE_VALID = 1,
    C3DB_CACHE_DELETED = 2,
    C3DB_CACHE_CORRUPT = 3
  };

  //! Metadata stored at the beginning of every cache entry.
  //!
  //! The complete cache-entry layout is:
  //!
  //!   CACHE_META | CACHED_REC
  //!
  //! CACHE_META is used by cache lookup and replacement policy. CACHED_REC is
  //! the compact c3db_cached_rec_t representation stored immediately after it.
  struct __attribute__((packed)) cache_meta_t
  {
    uint8_t state;
    c3db_id_t id;
  };

  c3db_err_t allocate_cache(c3db_cache_mode_t mode);
  //! Splits cache_memory_ into sequential, historical and write-cache zones.
  void configure_cache(c3db_cache_mode_t mode);
  void release_cache();

private:
  size_t hist_capacity() const;

  size_t get_entry_idx_from_seq_pos(size_t pos) const;
  size_t get_entry_idx_from_hist_pos(size_t pos) const;
  size_t get_entry_idx_from_write_pos(size_t pos) const;

  //! Entry layout helpers for the CACHE_META | CACHED_REC memory format.
  cache_meta_t* get_meta(size_t entry_num);
  const cache_meta_t* get_meta(size_t entry_num) const;
  c3db_cached_rec_t get_rec(size_t entry_num);
  c3db_cached_rec_t get_rec(size_t entry_num) const;

  //! Cache-entry materialization and mutation helpers.
  void set_entry(size_t entry_num, cache_entry_state_t state, c3db_id_t id, const uint8_t* payload);
  c3db_err_t set_entry(size_t entry_num, c3db_id_t id, const c3db_db_rec_t &rec);

  //! Cache lookup helpers.
  bool row_in_seq_cache(uint32_t row_id) const;
  bool row_in_write_cache(uint32_t row_id) const;
  //! Finds the newest historical entry matching id, including negative hits.
  bool find_hist_entry(c3db_id_t id, size_t &index) const;
  bool matchs_entry(size_t entry_num, c3db_id_t id) const;

  //! Invalidates cached entries for a physical row after free-list reuse.
  void mark_row_empty(uint32_t row_id);

  //! Cache-zone reset helpers.
  void clear_seq_cache();
  void clear_hist_cache();
  void clear_write_cache();
  void clear_seq_access();

  c3db_err_t read_entry_payload(size_t entry_num, uint8_t* payload) const;
  //! Loads seq_cache_ from row_id, moving useful previous seq entries to history.
  c3db_err_t load_seq_cache(uint32_t row_id);

  c3db_err_t update_from_cached_entry(size_t entry_num, c3db_id_t id, const uint8_t* payload);
  c3db_err_t remove_from_cached_entry(size_t entry_num, c3db_id_t id);

  //! Copies recently accessed valid sequential entries into hist_cache_.
  c3db_err_t move_seq_to_hist();
  void add_hist_entry(const cache_meta_t* meta);
  void register_seq_access(size_t seq_index);

  /*
   * Cache storage model:
   *
   * cache_memory_ owns the only block that stores cached entries. Entries are
   * addressed by an absolute entry number in this block:
   *
   *   seq:   [0, seq_capacity_)
   *   hist:  [seq_capacity_, seq_capacity_ + hist_capacity())
   *   write: [seq_capacity_ + hist_capacity(), total_capacity_)
   */

  //! Requested cache memory in bytes, rounded down to complete entries on allocation.
  size_t memory_size_;
  //! Size in bytes of one CACHE_META | CACHED_REC entry.
  size_t entry_size_;
  //! Total number of complete entries available in cache_memory_.
  size_t total_capacity_;
  //! Current cache partitioning mode.
  c3db_cache_mode_t mode_;
  //! Single contiguous block that stores seq_cache_, hist_cache_ and write_cache_ entries.
  uint8_t* cache_memory_;

  //! Number of entries assigned to seq_cache_.
  size_t seq_capacity_;
  //! Physical row id represented by seq_cache_[0].
  uint32_t seq_first_row_;
  //! Number of loaded entries currently valid in seq_cache_.
  size_t seq_rows_loaded_;

  size_t hist_pos_;
  size_t hist_rows_loaded_;

  //! Number of entries assigned to write_cache_.
  size_t write_capacity_;
  //! Physical row id reserved for write_cache_[0].
  uint32_t write_first_row_;
  //! Number of pending append entries stored in write_cache_.
  size_t write_rows_pending_;

  /*
   * Sequential-access history:
   *
   * seq_access_ records seq_cache_ indices read by select(). When the
   * sequential window is replaced, move_seq_to_hist() copies the most recently
   * accessed valid entries to hist_cache_. seq_copied_ is a temporary bitmap to
   * avoid copying the same seq entry more than once.
   */
  size_t* seq_access_;
  uint8_t* seq_copied_;
  size_t seq_access_pos_;
  size_t seq_access_count_;
};
