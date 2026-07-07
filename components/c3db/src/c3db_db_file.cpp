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

#include "c3db_db_file.h"
#include "c3db_cached_db_file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdio.h>

#include "c3db_config.h"
#include "c3db_defs.h"
#include "c3db_utils.h"

static constexpr uint8_t C3DB_DB_MAGIC[4] = {'C', '3', 'D', 'B'};
static constexpr uint32_t C3DB_DB_VERSION = 4;
static constexpr uint32_t C3DB_INITIAL_CYCLE = 0;
static constexpr uint8_t C3DB_SLOT_0 = 0;
static constexpr uint8_t C3DB_SLOT_1 = 1;
static constexpr uint32_t C3DB_CTRL_FREE_INITIAL_CRC = 0x7AADAA96u;


/* ==========================================================================
 * Local helpers
 * ========================================================================== */

static bool ctrl_seq_newer(uint32_t a, uint32_t b) {
  /*
   * Modular comparison keeps normal uint32_t wraparound usable. It assumes two
   * versions of the same group are never separated by more than half the range,
   * which is realistic for alternating persistent writes.
   */
  return static_cast<int32_t>(a - b) > 0;
}

/* ==========================================================================
 * c3db_ctrl_free_t
 * ========================================================================== */

struct __attribute__((packed)) c3db_ctrl_free_t
{
  //! CRC32 for the initial persistent free-list state below.
  uint32_t crc = C3DB_CTRL_FREE_INITIAL_CRC;
  //! Sequence 1 marks the control as initialized.
  uint32_t seq = 1;
  //! No deleted row is available initially.
  uint32_t first = UINT32_MAX;
  //! The initial free-list is empty.
  uint32_t count = 0;
};


/* ==========================================================================
 * c3db_header_t
 * ========================================================================== */

/**
 * DBF persistent header controller.
 *
 * The header owns the physical layout of DBF metadata and hides the A/B
 * selection of the free-list control. c3db_db_file_t only sees the active
 * free-list values and stack-like operations: add, pop and reset.
 */
struct c3db_header_t
{
private:
  static constexpr size_t MAGIC_OFFSET = 0;
  static constexpr size_t VERSION_OFFSET = MAGIC_OFFSET + sizeof(C3DB_DB_MAGIC);
  static constexpr size_t REPAIR_OFFSET = VERSION_OFFSET + sizeof(uint32_t);
  static constexpr size_t CTRL_FREE_OFFSET = REPAIR_OFFSET + sizeof(uint8_t);

  c3db_file_t* file_;
  uint8_t magic_[sizeof(C3DB_DB_MAGIC)];
  uint32_t version_;
  uint8_t repair_;
  c3db_ctrl_free_t ctrl_free_[2];
  uint8_t read_ctrl_idx_;

public:
  //! Creates an empty DBF header controller.
  c3db_header_t()
    : file_(nullptr),
      magic_{},
      version_(0),
      repair_(0),
      ctrl_free_{},
      read_ctrl_idx_(0) {
  }

  c3db_header_t(const c3db_header_t&) = delete;
  c3db_header_t& operator=(const c3db_header_t&) = delete;

  static constexpr size_t size() {
    return CTRL_FREE_OFFSET + (2 * sizeof(c3db_ctrl_free_t));
  }

  //! Returns the DBF header version read from disk.
  uint32_t version() const {
    return version_;
  }

  //! Returns the first reusable physical row in the active free-list.
  uint32_t first_free() const {
    return ctrl_r()->first;
  }

  //! Returns the number of reusable physical rows in the active free-list.
  uint32_t free_count() const {
    return ctrl_r()->count;
  }

  //! Returns true when a full free-list repair is pending.
  bool repair_needed() const {
    return repair_ != 0;
  }

  //! Persists the free-list repair flag.
  c3db_err_t repair_needed(bool value) {
    if (!file_) return C3DB_FILE_NOT_OPEN_ERR;
    repair_ = value ? 1u : 0u;
    return file_->write_hdr(REPAIR_OFFSET, &repair_, sizeof(repair_));
  }

  //! Attaches the controller to an opened file and loads or initializes header metadata.
  c3db_err_t begin(c3db_file_t* file) {
    if (!file) return C3DB_INVALID_ARG_ERR;
    file_ = file;

    ON_ERR_RETURN(file_->read_hdr(MAGIC_OFFSET, magic_, sizeof(magic_)));
    const uint8_t empty_magic[sizeof(magic_)] = {};
    if (std::memcmp(magic_, empty_magic, sizeof(magic_)) == 0) {
      std::memcpy(magic_, C3DB_DB_MAGIC, sizeof(magic_));
      version_ = C3DB_DB_VERSION;
      repair_ = 0;
      read_ctrl_idx_ = 0;
      ctrl_free_[0] = c3db_ctrl_free_t{};
      ctrl_free_[1] = c3db_ctrl_free_t{};
      return write_hdr();
    }

    ON_ERR_RETURN(file_->read_hdr(VERSION_OFFSET, reinterpret_cast<uint8_t*>(&version_), sizeof(version_)));
    ON_ERR_RETURN(file_->read_hdr(REPAIR_OFFSET, &repair_, sizeof(repair_)));
    ON_ERR_RETURN(file_->read_hdr(CTRL_FREE_OFFSET, reinterpret_cast<uint8_t*>(ctrl_free_), sizeof(ctrl_free_)));

    if (std::memcmp(magic_, C3DB_DB_MAGIC, sizeof(magic_)) != 0 ||
        version_ != C3DB_DB_VERSION) {
      return C3DB_FILE_CORRUPT_ERR;
    }

    const bool valid_0 = ctrl_is_valid(ctrl_free_[0]);
    const bool valid_1 = ctrl_is_valid(ctrl_free_[1]);
    if (valid_0 && valid_1) {
      read_ctrl_idx_ = ctrl_seq_newer(ctrl_free_[1].seq, ctrl_free_[0].seq) ? 1 : 0;
      return C3DB_OK;
    }

    if (valid_0 || valid_1) {
      read_ctrl_idx_ = valid_0 ? 0 : 1;
      return C3DB_OK;
    }

    read_ctrl_idx_ = 0;
    return C3DB_FREE_LIST_REPAIR_WRN;
  }

  //! Pushes a reusable row to the free-list stack.
  c3db_err_t add_free(uint32_t row_id) {
    return write_free(row_id, ctrl_r()->count + 1);
  }

  //! Pops the current free-list head and sets the next row as new head.
  c3db_err_t pop_free(uint32_t next_first) {
    if (ctrl_r()->count == 0) return C3DB_FREE_LIST_EMPTY_ERR;
    return write_free(next_first, ctrl_r()->count - 1);
  }

  //! Rebuilds both free-list control copies from known-good values.
  c3db_err_t reset_free(uint32_t first, uint32_t count) {
    if (!file_) return C3DB_FILE_NOT_OPEN_ERR;

    init_ctrl(ctrl_free_[0], 1, first, count);
    init_ctrl(ctrl_free_[1], 2, first, count);
    read_ctrl_idx_ = 1;
    return file_->write_hdr(
      CTRL_FREE_OFFSET,
      reinterpret_cast<const uint8_t*>(ctrl_free_),
      sizeof(ctrl_free_)
    );
  }


private:
  const c3db_ctrl_free_t* ctrl_r() const {
    return &ctrl_free_[read_ctrl_idx_];
  }

  const c3db_ctrl_free_t* ctrl_w() const {
    return &ctrl_free_[1 - read_ctrl_idx_];
  }

  c3db_ctrl_free_t* ctrl_w() {
    return &ctrl_free_[1 - read_ctrl_idx_];
  }

  //! Writes the inactive free-list control copy and makes it active.
  c3db_err_t write_free(uint32_t first, uint32_t count) {
    if (!file_) return C3DB_FILE_NOT_OPEN_ERR;

    const uint8_t write_idx = 1 - read_ctrl_idx_;
    c3db_ctrl_free_t &ctrl = *ctrl_w();
    ctrl.seq = ctrl_r()->seq + 1;
    if (ctrl.seq == 0) ctrl.seq = 1;
    ctrl.first = first;
    ctrl.count = count;
    ctrl.crc = get_crc(ctrl);

    ON_ERR_RETURN(file_->write_hdr(
      CTRL_FREE_OFFSET + (write_idx * sizeof(c3db_ctrl_free_t)),
      reinterpret_cast<const uint8_t*>(&ctrl),
      sizeof(ctrl)
    ));

    read_ctrl_idx_ = write_idx;
    return C3DB_OK;
  }

  static uint32_t get_crc(const c3db_ctrl_free_t &ctrl) {
    return c3db_crc32(
      reinterpret_cast<const uint8_t*>(&ctrl.seq),
      sizeof(ctrl) - sizeof(ctrl.crc)
    );
  }

  static bool ctrl_is_valid(const c3db_ctrl_free_t &ctrl) {
    return ctrl.seq != 0 && ctrl.crc == get_crc(ctrl);
  }

  static void init_ctrl(c3db_ctrl_free_t &ctrl, uint32_t seq, uint32_t first = UINT32_MAX, uint32_t count = 0) {
    ctrl.seq = seq;
    ctrl.first = first;
    ctrl.count = count;
    ctrl.crc = get_crc(ctrl);
  }

  c3db_err_t write_hdr() {
    ON_ERR_RETURN(file_->write_hdr(MAGIC_OFFSET, magic_, sizeof(magic_)));
    ON_ERR_RETURN(file_->write_hdr(VERSION_OFFSET, reinterpret_cast<const uint8_t*>(&version_), sizeof(version_)));
    ON_ERR_RETURN(file_->write_hdr(REPAIR_OFFSET, &repair_, sizeof(repair_)));
    return file_->write_hdr(
      CTRL_FREE_OFFSET,
      reinterpret_cast<const uint8_t*>(ctrl_free_),
      sizeof(ctrl_free_)
    );
  }
};


/* ==========================================================================
 * c3db_rec_t
 * ========================================================================== */

c3db_rec_t::c3db_rec_t(uint8_t* raw_data, size_t data_size)
  : data_size_(data_size),
    raw_data_(raw_data) {
}

uint8_t* c3db_rec_t::bytes() {
  return raw_data_;
}

const uint8_t* c3db_rec_t::bytes() const {
  return raw_data_;
}

c3db_err_t c3db_rec_t::write_deleted(c3db_file_t* file, size_t row_id) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  return file->write_rec(row_id, 0, reinterpret_cast<const uint8_t*>(del_block()), sizeof(del_block_t));
}

c3db_err_t c3db_rec_t::save_deleted(c3db_file_t* file, size_t row_id, uint32_t next_free) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;

  const del_block_t old_delete_block = *del_block();
  const uint8_t old_state = state();

  ON_ERR_RETURN(set_deleted(next_free));

  c3db_err_t err = write_deleted(file, row_id);
  if (IS_ERR(err)) {
    *del_block() = old_delete_block;
    set_state(old_state);
  }
  return err;
}

bool c3db_rec_t::is_valid() const {
  return state() != C3DB_REC_CORRUPT;
}

bool c3db_rec_t::is_active() const {
  return state() == C3DB_REC_ACTIVE;
}

bool c3db_rec_t::is_deleted() const {
  return state() == C3DB_REC_DELETED;
}

uint32_t c3db_rec_t::next_free() const {
  return del_block()->next_free;
}

uint32_t c3db_rec_t::cycle() const {
  const data_block_t* block = data_block();
  return is_active() && block ? block->cycle : UINT32_MAX;
}

uint32_t c3db_rec_t::get_last_cycle() const {
  const data_block_t* block = data_block();
  return block ? block->cycle : UINT32_MAX;
}

uint32_t c3db_rec_t::get_next_cycle() const {
  const uint32_t current_cycle = get_last_cycle();
  if (current_cycle == UINT32_MAX) return UINT32_MAX;
  if (current_cycle == UINT32_MAX - 1) return 0;
  return current_cycle + 1;
}

uint8_t c3db_rec_t::next_data_slot() const {
  return data_slot() == C3DB_REC_SLOT_0 ? C3DB_REC_SLOT_1 : C3DB_REC_SLOT_0;
}

uint32_t c3db_rec_t::get_last_seq() const {
  if (!is_valid()) return UINT32_MAX;

  const uint32_t del_seq = del_block()->seq;
  const data_block_t* block = data_block();
  if (!block) return del_seq;

  const uint32_t data_seq = block->seq;
  if (del_seq == 0) return data_seq;
  if (data_seq == 0) return del_seq;
  return seq_newer(del_seq, data_seq) ? del_seq : data_seq;
}

c3db_err_t c3db_rec_t::get_next_seq(uint32_t &seq) const {
  const uint32_t current = get_last_seq();
  if (current == UINT32_MAX) return C3DB_REC_CORRUPT_ERR;
  seq = current + 1;
  return C3DB_OK;
}

c3db_err_t c3db_rec_t::initialize(const uint8_t* payload) {
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (!payload) return C3DB_INVALID_ARG_ERR;

  std::memset(raw_data_, 0, rec_size());
  reset_state();
  ON_ERR_RETURN(set_payload(1, 0, payload));
  set_state(C3DB_REC_ACTIVE);
  set_data_slot(C3DB_REC_SLOT_0);
  return C3DB_OK;
}

c3db_err_t c3db_rec_t::set_deleted(uint32_t next_free) {
  // The caller must ensure this state transition is valid for the operation.
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (!is_valid()) return C3DB_REC_CORRUPT_ERR;

  uint32_t seq = 0;
  ON_ERR_RETURN(get_next_seq(seq));

  del_block_t state = {
    .seq = seq,
    .next_free = next_free
  };
  state.crc = del_block_crc(state);
  *del_block() = state;
  set_state(C3DB_REC_DELETED);
  return C3DB_OK;
}

c3db_err_t c3db_rec_t::set_payload(const uint8_t* payload) {
  // The caller must ensure this state transition is valid for the operation.
  if (!payload) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (!is_active() && !is_deleted()) return C3DB_REC_CORRUPT_ERR;

  uint32_t seq = 0;
  ON_ERR_RETURN(get_next_seq(seq));

  const uint32_t new_cycle = is_deleted() ? get_next_cycle() : cycle();
  if (new_cycle == UINT32_MAX) return C3DB_REC_CORRUPT_ERR;
  return set_payload(seq, new_cycle, payload);
}

uint32_t c3db_rec_t::del_block_crc(const del_block_t &state) {
  return c3db_crc32(
    reinterpret_cast<const uint8_t*>(&state.seq),
    sizeof(state) - sizeof(state.crc)
  );
}

bool c3db_rec_t::del_block_is_valid(const del_block_t &state) {
  return state.seq != 0 && state.crc == del_block_crc(state);
}

uint32_t c3db_rec_t::data_block_crc(
  uint32_t seq,
  uint32_t cycle,
  const uint8_t* payload,
  size_t data_size
) {
  uint32_t crc = C3DB_CRC32_INIT;
  crc = c3db_crc32_update(crc, reinterpret_cast<const uint8_t*>(&seq), sizeof(seq));
  crc = c3db_crc32_update(crc, reinterpret_cast<const uint8_t*>(&cycle), sizeof(cycle));
  crc = c3db_crc32_update(crc, payload, data_size);
  return c3db_crc32_finish(crc);
}

bool c3db_rec_t::data_block_is_valid(
  const data_block_t &slot,
  const uint8_t* payload,
  size_t data_size
) {
  return slot.seq != 0 && slot.crc == data_block_crc(slot.seq, slot.cycle, payload, data_size);
}

bool c3db_rec_t::seq_newer(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

void c3db_rec_t::reset_state() {
  set_state(C3DB_REC_CORRUPT);
  set_data_slot(C3DB_REC_NO_SLOT);
}

/* ==========================================================================
 * c3db_db_rec_t
 * ==========================================================================
 */

c3db_db_rec_t::c3db_db_rec_t(uint8_t* raw_data, size_t data_size)
  : c3db_rec_t(raw_data, data_size),
    state_(C3DB_REC_CORRUPT),
    data_slot_(C3DB_REC_NO_SLOT) {
}

c3db_err_t c3db_db_rec_t::read_rec(c3db_file_t* file, size_t row_id) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  return file->select(row_id, raw_data_);
}

c3db_err_t c3db_db_rec_t::write_rec(c3db_file_t* file, size_t row_id) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  return file->update(row_id, raw_data_);
}

c3db_err_t c3db_db_rec_t::add_rec(c3db_file_t* file, size_t &row_id) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  return file->append(raw_data_, row_id);
}

c3db_err_t c3db_db_rec_t::parse() {
  if (!raw_data_) return C3DB_GENERIC_ERR;
  reset_state();

  const bool delete_valid = del_block_is_valid(*del_block());
  uint8_t slot = C3DB_REC_NO_SLOT;
  const bool slot_valid = newest_valid_slot(slot);

  if (delete_valid && slot_valid) {
    set_data_slot(slot);
    set_state(seq_newer(del_block()->seq, data_block(slot)->seq) ? C3DB_REC_DELETED : C3DB_REC_ACTIVE);
    return C3DB_OK;
  }

  if (slot_valid) {
    set_state(C3DB_REC_ACTIVE);
    set_data_slot(slot);
    return C3DB_OK;
  }

  if (delete_valid) {
    set_state(C3DB_REC_DELETED);
    return C3DB_OK;
  }

  set_state(C3DB_REC_CORRUPT);
  return C3DB_OK;
}

uint8_t c3db_db_rec_t::state() const {
  return state_;
}

auto c3db_db_rec_t::del_block() -> del_block_t* {
  return reinterpret_cast<del_block_t*>(raw_data_);
}

auto c3db_db_rec_t::del_block() const -> const del_block_t* {
  return reinterpret_cast<const del_block_t*>(raw_data_);
}

auto c3db_db_rec_t::data_block() -> data_block_t* {
  const uint8_t slot = data_slot();
  if (slot != C3DB_REC_SLOT_0 && slot != C3DB_REC_SLOT_1) return nullptr;
  if (!is_valid()) return nullptr;
  return data_block(slot);
}

auto c3db_db_rec_t::data_block() const -> const data_block_t* {
  const uint8_t slot = data_slot();
  if (slot != C3DB_REC_SLOT_0 && slot != C3DB_REC_SLOT_1) return nullptr;
  if (!is_valid()) return nullptr;
  return data_block(slot);
}

uint8_t c3db_db_rec_t::data_slot() const {
  return data_slot_;
}

void c3db_db_rec_t::set_state(uint8_t state) {
  state_ = state;
}

void c3db_db_rec_t::set_data_slot(uint8_t data_slot) {
  data_slot_ = data_slot;
}

c3db_err_t c3db_db_rec_t::save_payload(c3db_file_t* file, size_t row_id, const uint8_t* payload) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!payload) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (!is_active() && !is_deleted()) return C3DB_REC_CORRUPT_ERR;
  if (data_slot() != C3DB_REC_SLOT_0 && data_slot() != C3DB_REC_SLOT_1) return C3DB_REC_CORRUPT_ERR;

  const uint8_t write_slot = next_data_slot();

  c3db_err_t err = set_payload(payload);
  if (OK(err)) err = write_payload(file, row_id);

  if (OK(err)) {
    set_data_slot(write_slot);
    set_state(C3DB_REC_ACTIVE);
  }

  return err;
}

c3db_err_t c3db_db_rec_t::get_payload(uint8_t* buffer) const {
  // The caller must ensure reading this record state is valid for the operation.
  if (!buffer) return C3DB_INVALID_ARG_ERR;
  if (!is_valid()) return C3DB_REC_CORRUPT_ERR;
  const uint8_t slot = data_slot();
  if (slot != C3DB_REC_SLOT_0 && slot != C3DB_REC_SLOT_1) return C3DB_REC_CORRUPT_ERR;

  std::memcpy(buffer, payload(slot), data_size_);
  return C3DB_OK;
}

c3db_err_t c3db_db_rec_t::export_payload(c3db_file_t* target_file) const {
  if (!target_file) return C3DB_INVALID_ARG_ERR;
  if (!is_active()) return C3DB_REC_CORRUPT_ERR;
  const uint8_t slot = data_slot();
  if (slot != C3DB_REC_SLOT_0 && slot != C3DB_REC_SLOT_1) return C3DB_REC_CORRUPT_ERR;
  return target_file->write(payload(slot), data_size_);
}

c3db_err_t c3db_db_rec_t::export_to(c3db_cached_rec_t &cached_rec) const {
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (!is_valid()) return C3DB_REC_CORRUPT_ERR;

  uint8_t slot = C3DB_REC_NO_SLOT;
  if (!newest_valid_slot(slot)) return C3DB_REC_CORRUPT_ERR;

  return cached_rec.initialize(state(), slot, *del_block(), *data_block(slot), payload(slot));
}

size_t c3db_db_rec_t::rec_size() const {
  return rec_size(data_size_);
}

size_t c3db_db_rec_t::rec_size(size_t data_size) {
  return sizeof(del_block_t) +
         (2 * (sizeof(data_block_t) + data_size));
}

size_t c3db_db_rec_t::data_block_size() const {
  return sizeof(data_block_t) + data_size_;
}

size_t c3db_db_rec_t::slot_offset(uint8_t slot) const {
  return sizeof(del_block_t) +
         (slot * data_block_size());
}

const uint8_t* c3db_db_rec_t::payload(uint8_t slot) const {
  return raw_data_ + slot_offset(slot) + sizeof(data_block_t);
}

auto c3db_db_rec_t::data_block(uint8_t slot) -> data_block_t* {
  return reinterpret_cast<data_block_t*>(raw_data_ + slot_offset(slot));
}

auto c3db_db_rec_t::data_block(uint8_t slot) const -> const data_block_t* {
  return reinterpret_cast<const data_block_t*>(raw_data_ + slot_offset(slot));
}

c3db_err_t c3db_db_rec_t::set_payload(
  uint32_t seq,
  uint32_t cycle,
  const uint8_t* payload
) {
  // The caller must ensure this state transition is valid for the operation.
  if (!payload) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;

  const uint8_t slot = next_data_slot();
  set_payload(slot, seq, cycle, payload);
  return C3DB_OK;
}

c3db_err_t c3db_db_rec_t::write_payload(c3db_file_t* file, size_t row_id) {
  if (!file) return C3DB_INVALID_ARG_ERR;
  if (!raw_data_) return C3DB_GENERIC_ERR;
  if (data_slot() != C3DB_REC_SLOT_0 && data_slot() != C3DB_REC_SLOT_1) return C3DB_REC_CORRUPT_ERR;

  const uint8_t slot = next_data_slot();
  const size_t offset = sizeof(del_block_t) + (slot * data_block_size());
  return file->write_rec(
    row_id,
    offset,
    reinterpret_cast<const uint8_t*>(data_block(slot)),
    data_block_size()
  );
}

void c3db_db_rec_t::set_payload(
  uint8_t slot,
  uint32_t seq,
  uint32_t cycle,
  const uint8_t* payload
) {
  data_block_t* slot_ptr = data_block(slot);
  slot_ptr->seq = seq;
  slot_ptr->cycle = cycle;
  slot_ptr->crc = data_block_crc(seq, cycle, payload, data_size_);
  std::memcpy(reinterpret_cast<uint8_t*>(slot_ptr) + sizeof(data_block_t), payload, data_size_);
}

bool c3db_db_rec_t::newest_valid_slot(uint8_t &slot) const {
  const bool slot_0_newer = seq_newer(data_block(C3DB_REC_SLOT_0)->seq, data_block(C3DB_REC_SLOT_1)->seq);
  const uint8_t first_slot = slot_0_newer ? C3DB_REC_SLOT_0 : C3DB_REC_SLOT_1;
  const uint8_t second_slot = slot_0_newer ? C3DB_REC_SLOT_1 : C3DB_REC_SLOT_0;

  if (data_block_is_valid(*data_block(first_slot), payload(first_slot), data_size_)) {
    slot = first_slot;
    return true;
  }
  if (data_block_is_valid(*data_block(second_slot), payload(second_slot), data_size_)) {
    slot = second_slot;
    return true;
  }
  return false;
}

/* ==========================================================================
 * c3db_db_file_t
 * ========================================================================== */

c3db_db_file_t::c3db_db_file_t(size_t data_size)
  : data_size_(data_size),
    file_(),
    header_(nullptr),
    first_free_(C3DB_NULL_REF),
    free_count_(0),
    free_list_repair_needed_(false),
    rec_buf_(nullptr),
    rec_(nullptr),
    cached_row_(C3DB_NULL_REF) {
}

c3db_db_file_t::~c3db_db_file_t() {
  end();
}

c3db_err_t c3db_db_file_t::create(const char* base_file_name, const char* extension) {
  if (data_size_ == 0) return C3DB_INVALID_ARG_ERR;
  if (rec_size() > C3DB_SHARED_BUFFER_SIZE) return C3DB_INVALID_ARG_ERR;
  if (file_.is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;

  char* dbf_file_name = nullptr;
  ON_ERR_RETURN(c3db_make_file_name(base_file_name, extension, dbf_file_name));

  c3db_err_t err = file_.create(dbf_file_name, 0, c3db_header_t::size(), rec_size());
  delete[] dbf_file_name;

  if (OK(err)) {
    rec_buf_ = new (std::nothrow) uint8_t[rec_size()];
    if (!rec_buf_) err = C3DB_GENERIC_ERR;
    if (OK(err)) {
      rec_ = new (std::nothrow) c3db_db_rec_t(rec_buf_, data_size_);
      if (!rec_) err = C3DB_GENERIC_ERR;
    }
  }
  if (OK(err)) err = load_header();

  if (IS_ERR(err)) {
    end();
    return err;
  }

  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::begin(const char* base_file_name, bool read_only, const char* extension) {
  if (data_size_ == 0) return C3DB_INVALID_ARG_ERR;
  if (rec_size() > C3DB_SHARED_BUFFER_SIZE) return C3DB_GENERIC_ERR;
  if (file_.is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;

  char* dbf_file_name = nullptr;
  ON_ERR_RETURN(c3db_make_file_name(base_file_name, extension, dbf_file_name));

  c3db_err_t err = file_.begin(dbf_file_name, read_only, 0, c3db_header_t::size(), rec_size());
  delete[] dbf_file_name;

  if (IS_ERR(err)) return err;
  return init_dbf();
}

c3db_err_t c3db_db_file_t::begin(FILE* file, bool read_only) {
  if (data_size_ == 0) return C3DB_INVALID_ARG_ERR;
  if (rec_size() > C3DB_SHARED_BUFFER_SIZE) return C3DB_GENERIC_ERR;
  if (file_.is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;

  ON_ERR_RETURN(file_.begin(file, read_only, 0, c3db_header_t::size(), rec_size()));
  return init_dbf();
}

c3db_err_t c3db_db_file_t::end() {
  c3db_err_t err = file_.end();
  delete rec_;
  rec_ = nullptr;
  delete[] rec_buf_;
  rec_buf_ = nullptr;
  reset_state();
  return err;
}

c3db_err_t c3db_db_file_t::append(const uint8_t* data, c3db_id_t &id) {
  if (!file_.is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (file_.is_read_only()) return C3DB_READ_ONLY_ERR;
  return append_data(data, id);
}

c3db_err_t c3db_db_file_t::insert(const uint8_t* data, c3db_id_t &id) {
  if (!file_.is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!data) return C3DB_INVALID_ARG_ERR;
  if (file_.is_read_only()) return C3DB_READ_ONLY_ERR;

  if (first_free_ == C3DB_NULL_REF) return append_data(data, id);

  const size_t physical_count = file_.rec_count();
  const uint32_t row_id = first_free_;

  if (row_id >= physical_count || free_count_ == 0) {
    /*
     * The free-list is no longer trustworthy. We discard only that global list,
     * keep the DBF usable with a fresh list and report that a full repair is
     * pending so lost deleted slots can be recovered later.
     */
    ON_ERR_RETURN(set_free_list_repair_needed(true));
    c3db_err_t append_err = append_data(data, id);
    return IS_ERR(append_err) ? append_err : C3DB_FREE_LIST_REPAIR_WRN;
  }

  c3db_err_t err = read_record(row_id);
  if (IS_ERR(err)) return err;

  const bool next_ok = rec_->next_free() == C3DB_NULL_REF ||
                       rec_->next_free() < physical_count;
  if (!rec_->is_deleted() || !next_ok) {
    /*
     * A free-list entry must point to a deleted row and to another valid row
     * or null. Anything else means reuse would risk touching live data.
     */
    ON_ERR_RETURN(set_free_list_repair_needed(true));
    c3db_err_t append_err = append_data(data, id);
    return IS_ERR(append_err) ? append_err : C3DB_FREE_LIST_REPAIR_WRN;
  }

  const uint32_t new_first_free = rec_->next_free();

  /*
   * Remove the row from the global free-list before reactivating it. A reset
   * after this point may orphan the slot, but the free-list will not point to
   * a record that can become active.
   */
  ON_ERR_RETURN(pop_free(new_first_free));

  ON_ERR_RETURN(rec_->save_payload(&file_, row_id, data));

  id = c3db_id(row_id, rec_->cycle());
  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::import_file(FILE* source_file, c3db_id_t &first_id, size_t &rows_added) {
  if (!file_.is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!source_file) return C3DB_INVALID_ARG_ERR;
  if (file_.is_read_only()) return C3DB_READ_ONLY_ERR;
  if (data_size_ == 0) return C3DB_INVALID_ARG_ERR;

  const size_t physical_size = rec_size();
  if (physical_size == 0 || physical_size > C3DB_SHARED_BUFFER_SIZE) return C3DB_UNSUPPORTED_OP_ERR;

  first_id = C3DB_NULL_ID;
  rows_added = 0;

  const long source_start = ftell(source_file);
  if (source_start < 0) return C3DB_FILE_TELL_ERR;
  if (fseek(source_file, 0, SEEK_END)) return C3DB_FILE_SEEK_ERR;
  const long source_end = ftell(source_file);
  if (source_end < 0) return C3DB_FILE_TELL_ERR;
  if (source_end < source_start) return C3DB_FILE_SIZE_ERR;
  if (fseek(source_file, source_start, SEEK_SET)) return C3DB_FILE_SEEK_ERR;

  const size_t source_size = static_cast<size_t>(source_end - source_start);
  if ((source_size % data_size_) != 0) return C3DB_FILE_SIZE_ERR;

  size_t rows_remaining = source_size / data_size_;
  const size_t rows_per_chunk = C3DB_SHARED_BUFFER_SIZE / physical_size;
  cached_row_ = C3DB_NULL_REF;

  /*
   * Import is logical, not a raw physical copy. Payloads are read in batches
   * and expanded into DBF physical records inside the shared buffer, so slot
   * state, CRCs and cycles are valid without doing one disk append per row.
   */
  while (rows_remaining > 0) {
    const size_t rows_now = rows_remaining < rows_per_chunk ? rows_remaining : rows_per_chunk;
    const size_t payload_bytes = rows_now * data_size_;

    if (fread(c3db_shared_buffer, 1, payload_bytes, source_file) != payload_bytes) {
      return feof(source_file) == 0 ? C3DB_FILE_READ_ERR : C3DB_EOF_ERR;
    }

    /*
     * Records are larger than payloads. Expanding from the end avoids
     * overwriting source payloads that have not been converted yet. rec_buf_
     * holds the current payload because initialize() clears the destination
     * record before writing metadata and data.
     */
    for (size_t i = rows_now; i > 0; --i) {
      const size_t index = i - 1;
      std::memcpy(rec_buf_, c3db_shared_buffer + (index * data_size_), data_size_);
      c3db_db_rec_t rec(c3db_shared_buffer + (index * physical_size), data_size_);
      ON_ERR_RETURN(rec.initialize(rec_buf_));
    }

    size_t chunk_first_row = 0;
    size_t chunk_rows_added = 0;
    ON_ERR_RETURN(file_.extend(c3db_shared_buffer, rows_now * physical_size, chunk_first_row, chunk_rows_added));
    if (chunk_rows_added != rows_now) return C3DB_FILE_WRITE_ERR;

    if (rows_added == 0) first_id = c3db_id(static_cast<uint32_t>(chunk_first_row), C3DB_INITIAL_CYCLE);
    rows_added += chunk_rows_added;
    rows_remaining -= rows_now;
  }

  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::import_file(
  const char* source_file_name,
  c3db_id_t &first_id,
  size_t &rows_added
) {
  if (!source_file_name || source_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  FILE* source_file = fopen(source_file_name, "r");
  if (!source_file) return C3DB_FILE_OPEN_ERR;

  c3db_err_t err = import_file(source_file, first_id, rows_added);
  if (fclose(source_file)) return IS_ERR(err) ? err : C3DB_FILE_CLOSE_ERR;
  return err;
}

c3db_err_t c3db_db_file_t::export_file(FILE* target_file, size_t &rows_exported) {
  if (!file_.is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!target_file) return C3DB_INVALID_ARG_ERR;

  c3db_file_t target;
  ON_ERR_RETURN(target.begin(target_file, false));
  return export_to(target, rows_exported);
}

c3db_err_t c3db_db_file_t::export_file(const char* target_file_name, size_t &rows_exported) {
  if (!target_file_name || target_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_file_t target;
  ON_ERR_RETURN(target.create(target_file_name));
  c3db_err_t err = export_to(target, rows_exported);
  c3db_err_t close_err = target.end();
  if (IS_ERR(err)) return err;
  if (IS_ERR(close_err)) return close_err;
  return KO(err) ? err : close_err;
}

c3db_err_t c3db_db_file_t::select(c3db_id_t id, uint8_t* data) {
  const uint32_t row_id = c3db_row(id);
  const uint32_t cycle = c3db_cycle(id);

  c3db_err_t err = read_record(row_id);
  if (IS_ERR(err)) return err;

  if (!rec_->is_active() || rec_->cycle() != cycle) return C3DB_REC_NOT_FOUND_ERR;
  return rec_->get_payload(data);
}

c3db_err_t c3db_db_file_t::update(c3db_id_t id, const uint8_t* data) {
  if (file_.is_read_only()) return C3DB_READ_ONLY_ERR;

  const uint32_t row_id = c3db_row(id);
  const uint32_t cycle = c3db_cycle(id);

  c3db_err_t err = read_record(row_id);
  if (IS_ERR(err)) return err;

  if (!rec_->is_active() || rec_->cycle() != cycle) return C3DB_REC_NOT_FOUND_ERR;

  /*
   * Updating does not overwrite the current payload. The inactive slot receives
   * the new payload with a newer sequence, so a partial write cannot destroy the
   * previous visible value.
   */
  return rec_->save_payload(&file_, row_id, data);
}

c3db_err_t c3db_db_file_t::remove(c3db_id_t id) {
  if (file_.is_read_only()) return C3DB_READ_ONLY_ERR;

  const uint32_t row_id = c3db_row(id);
  const uint32_t cycle = c3db_cycle(id);

  c3db_err_t err = read_record(row_id);
  if (IS_ERR(err)) return err;

  if (!rec_->is_active() || rec_->cycle() != cycle) return C3DB_REC_NOT_FOUND_ERR;

  ON_ERR_RETURN(rec_->save_deleted(&file_, row_id, first_free_));

  return add_free(row_id);
}

size_t c3db_db_file_t::data_size() const {
  return data_size_;
}

c3db_err_t c3db_db_file_t::max_rec_count(size_t &count) const {
  const size_t physical_count = file_.rec_count();
  if (free_list_repair_needed_) {
    count = physical_count;
    return C3DB_FREE_LIST_REPAIR_WRN;
  }
  if (free_count_ > physical_count) return C3DB_COUNT_UNKNOWN_ERR;

  count = physical_count - free_count_;
  return C3DB_OK;
}

bool c3db_db_file_t::free_list_repair_needed() const {
  return free_list_repair_needed_;
}

c3db_err_t c3db_db_file_t::repair_free_list(const char* base_file_name, const char* extension) {
  if (data_size_ == 0) return C3DB_INVALID_ARG_ERR;
  if (file_.is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;

  char* dbf_file_name = nullptr;
  ON_ERR_RETURN(c3db_make_file_name(base_file_name, extension, dbf_file_name));

  c3db_err_t err = file_.begin(dbf_file_name, false, 0, c3db_header_t::size(), rec_size());
  delete[] dbf_file_name;

  if (IS_ERR(err)) return err;
  return repair_free_list();
}

c3db_err_t c3db_db_file_t::repair_free_list(FILE* file) {
  if (data_size_ == 0) return C3DB_INVALID_ARG_ERR;
  if (file_.is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;

  ON_ERR_RETURN(file_.begin(file, false, 0, c3db_header_t::size(), rec_size()));
  return repair_free_list();
}

bool c3db_db_file_t::is_open() const {
  return file_.is_open();
}

bool c3db_db_file_t::is_read_only() const {
  return file_.is_read_only();
}

c3db_file_t& c3db_db_file_t::file() {
  return file_;
}

const c3db_file_t& c3db_db_file_t::file() const {
  return file_;
}

c3db_err_t c3db_db_file_t::read_record(size_t row_id, c3db_db_rec_t &rec) {
  if (!file_.is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (!rec.bytes()) return C3DB_INVALID_ARG_ERR;

  c3db_err_t err = rec.read_rec(&file_, row_id);
  if (err == C3DB_EOF_ERR) return C3DB_REC_NOT_FOUND_ERR;
  if (IS_ERR(err)) return err;

  return rec.parse();
}

c3db_err_t c3db_db_file_t::read_record(size_t row_id) {
  if (!rec_) return C3DB_GENERIC_ERR;
  if (cached_row_ == row_id) return C3DB_OK;

  c3db_err_t err = read_record(row_id, *rec_);
  if (IS_ERR(err)) {
    cached_row_ = C3DB_NULL_REF;
    return err;
  }

  cached_row_ = static_cast<uint32_t>(row_id);
  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::append_data(const uint8_t* data, c3db_id_t &id) {
  /*
   * A freshly appended row starts with one valid data slot. The unused groups
   * remain zeroed, which is the invalid/uninitialized marker.
   */
  if (!rec_) return C3DB_GENERIC_ERR;
  ON_ERR_RETURN(rec_->initialize(data));

  size_t row_id = 0;
  ON_ERR_RETURN(rec_->add_rec(&file_, row_id));
  cached_row_ = static_cast<uint32_t>(row_id);
  id = c3db_id(static_cast<uint32_t>(row_id), C3DB_INITIAL_CYCLE);
  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::add_free(uint32_t row_id) {
  if (file_.is_read_only()) return C3DB_READ_ONLY_ERR;
  if (!header_) return C3DB_FILE_NOT_OPEN_ERR;

  /*
   * The free-list is a stack. Removing a live row pushes it to the top after
   * the row's DELETE_BLOCK has already been written.
   */
  ON_ERR_RETURN(header_->add_free(row_id));
  first_free_ = row_id;
  ++free_count_;
  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::pop_free(uint32_t next_first) {
  if (file_.is_read_only()) return C3DB_READ_ONLY_ERR;
  if (!header_) return C3DB_FILE_NOT_OPEN_ERR;
  if (free_count_ == 0) return C3DB_FREE_LIST_EMPTY_ERR;

  /*
   * Reusing a deleted row pops the free-list before the row is reactivated. A
   * reset after this point can orphan the row, but cannot make the list point
   * to a row that may become active.
   */
  ON_ERR_RETURN(header_->pop_free(next_first));
  first_free_ = next_first;
  --free_count_;
  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::set_free_list_repair_needed(bool value) {
  /*
   * Enabling the repair flag deliberately discards the old free-list. The DBF
   * can keep accepting inserts/removes with a fresh list, while the persistent
   * flag keeps the lost-space condition visible.
   */
  if (value) {
    ON_ERR_RETURN(header_->repair_needed(true));
    ON_ERR_RETURN(header_->reset_free(C3DB_NULL_REF, 0));
    first_free_ = C3DB_NULL_REF;
    free_count_ = 0;
  } else {
    ON_ERR_RETURN(header_->repair_needed(false));
  }
  free_list_repair_needed_ = value;
  return C3DB_OK;
}

c3db_err_t c3db_db_file_t::export_to(c3db_file_t &target_file, size_t &rows_exported) {
  rows_exported = 0;
  if (data_size_ == 0) return C3DB_INVALID_ARG_ERR;

  const size_t physical_count = file_.rec_count();
  const bool batch_payloads = data_size_ <= C3DB_SHARED_BUFFER_SIZE;
  const size_t rows_per_batch = batch_payloads ? C3DB_SHARED_BUFFER_SIZE / data_size_ : 0;
  size_t rows_in_batch = 0;

  auto flush_batch = [&]() -> c3db_err_t {
    if (rows_in_batch == 0) return C3DB_OK;
    c3db_err_t err = target_file.write(c3db_shared_buffer, rows_in_batch * data_size_);
    rows_in_batch = 0;
    return err;
  };

  /*
   * Export walks physical rows and writes only active logical payloads. Deleted
   * or incomplete active rows are skipped: an interrupted write must not poison
   * a bulk export with data that is not fully validated.
  */
  for (size_t row = 0; row < physical_count; ++row) {
    ON_ERR_RETURN(read_record(row));

    if (!rec_->is_active()) continue;

    if (batch_payloads) {
      if (rows_in_batch == rows_per_batch) ON_ERR_RETURN(flush_batch());
      ON_ERR_RETURN(rec_->get_payload(c3db_shared_buffer + (rows_in_batch * data_size_)));
      ++rows_in_batch;
    } else {
      ON_ERR_RETURN(rec_->export_payload(&target_file));
    }

    ++rows_exported;
  }

  ON_ERR_RETURN(flush_batch());
  return target_file.flush();
}

c3db_err_t c3db_db_file_t::init_dbf() {
  c3db_err_t err = C3DB_OK;
  rec_buf_ = new (std::nothrow) uint8_t[rec_size()];
  if (!rec_buf_) err = C3DB_GENERIC_ERR;
  if (OK(err)) {
    rec_ = new (std::nothrow) c3db_db_rec_t(rec_buf_, data_size_);
    if (!rec_) err = C3DB_GENERIC_ERR;
  }
  if (OK(err)) err = load_header();

  if (IS_ERR(err)) {
    end();
  }

  return err;
}

c3db_err_t c3db_db_file_t::repair_free_list() {
  c3db_err_t err = C3DB_OK;
  /*
   * Public repair entry points reject already-open instances before calling
   * this helper. rec_buf_ is therefore owned by this repair pass and is always
   * released by end() before returning.
   */
  rec_buf_ = new (std::nothrow) uint8_t[rec_size()];
  if (!rec_buf_) err = C3DB_GENERIC_ERR;
  if (OK(err)) {
    rec_ = new (std::nothrow) c3db_db_rec_t(rec_buf_, data_size_);
    if (!rec_) err = C3DB_GENERIC_ERR;
  }

  header_ = new (std::nothrow) c3db_header_t();
  if (!header_) err = C3DB_GENERIC_ERR;
  if (OK(err)) err = header_->begin(&file_);
  if (IS_WNG(err)) err = C3DB_OK;

  uint32_t first_free = C3DB_NULL_REF;
  uint32_t free_count = 0;
  const size_t physical_count = file_.rec_count();

  /*
   * Repair trusts each row's local slot state and rebuilds only the global
   * free-list. Rows are linked in reverse scan order; ordering is irrelevant
   * because the list is only a pool of reusable slots.
  */
  for (size_t row = 0; OK(err) && row < physical_count; ++row) {
    err = read_record(row);
    if (OK(err) && rec_->is_deleted()) {
      if (row > UINT32_MAX) {
        err = C3DB_FILE_SIZE_ERR;
      } else {
        err = rec_->save_deleted(&file_, row, first_free);
        if (OK(err)) {
          first_free = static_cast<uint32_t>(row);
          ++free_count;
        }
      }
    }
  }

  if (OK(err)) {
    /*
     * Publish a fresh A/B control pair after every row link has been repaired.
     * Both copies are written so a reset after repair leaves no dependency on
     * whichever corrupted copy happened to survive before the scan.
     */
    err = header_->reset_free(first_free, free_count);
    if (OK(err)) err = header_->repair_needed(false);
  }

  c3db_err_t close_err = end();

  if (IS_ERR(err)) return err;
  if (IS_ERR(close_err)) return close_err;
  return KO(err) ? err : close_err;
}

c3db_err_t c3db_db_file_t::load_header() {
  delete header_;
  header_ = new (std::nothrow) c3db_header_t();
  if (!header_) return C3DB_GENERIC_ERR;

  c3db_err_t err = header_->begin(&file_);
  if (IS_ERR(err)) return err;

  if (err == C3DB_FREE_LIST_REPAIR_WRN) {
    /*
     * Records may still be readable by their local state groups. Losing both
     * free-list controls only makes deleted-slot reuse unreliable, so the DBF
     * starts a fresh list and keeps the repair need visible as a warning.
     */
    first_free_ = C3DB_NULL_REF;
    free_count_ = 0;
    if (!file_.is_read_only()) ON_ERR_RETURN(set_free_list_repair_needed(true));
    free_list_repair_needed_ = true;
    return C3DB_FREE_LIST_REPAIR_WRN;
  }

  first_free_ = header_->first_free();
  free_count_ = header_->free_count();
  free_list_repair_needed_ = header_->repair_needed();

  const size_t physical_count = file_.rec_count();
  if (free_count_ > physical_count ||
      (first_free_ != C3DB_NULL_REF && first_free_ >= physical_count)) {
    if (!file_.is_read_only()) ON_ERR_RETURN(set_free_list_repair_needed(true));
    first_free_ = C3DB_NULL_REF;
    free_count_ = 0;
    free_list_repair_needed_ = true;
    return C3DB_FREE_LIST_REPAIR_WRN;
  }

  if (free_list_repair_needed_) return C3DB_FREE_LIST_REPAIR_WRN;
  return C3DB_OK;
}

void c3db_db_file_t::reset_state() {
  delete header_;
  header_ = nullptr;
  first_free_ = C3DB_NULL_REF;
  free_count_ = 0;
  free_list_repair_needed_ = false;
  cached_row_ = C3DB_NULL_REF;
}

size_t c3db_db_file_t::rec_size() const {
  return c3db_db_rec_t::rec_size(data_size_);
}
