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

#include "c3db_index_ref.h"

#include "c3db_config.h"
#include "c3db_defs.h"
#include "c3db_file.h"
#include "c3db_utils.h"

c3db_index_ref_t::c3db_index_ref_t()
  : irf_(sizeof(c3db_ref_node_t)),
    first_free_ref_(C3DB_NULL_ID),
    read_only_(false) {
}

c3db_err_t c3db_index_ref_t::create(const char* base_file_name) {
  if (is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_err_t err = irf_.create(base_file_name, C3DB_INDEX_REF_EXTENSION);

  c3db_ref_node_t hdr = {};
  hdr.record_id = C3DB_NULL_ID;
  hdr.next_ref = C3DB_NULL_ID;

  c3db_id_t hdr_id = C3DB_NULL_ID;
  if (OK(err)) {
    /*
     * Record 0 uses the normal node layout but is reserved for IRF metadata.
     * record_id is unused; next_ref stores the head of the logical free-list.
     */
    err = irf_.append(reinterpret_cast<const uint8_t*>(&hdr), hdr_id);
    if (OK(err) && hdr_id != 0) err = C3DB_FILE_CORRUPT_ERR;
  }

  if (IS_ERR(err)) {
    irf_.end();
    delete_ref_file(base_file_name);
    reset_state();
    return err;
  }

  first_free_ref_ = C3DB_NULL_ID;
  read_only_ = false;
  return C3DB_OK;
}

c3db_err_t c3db_index_ref_t::begin(const char* base_file_name, bool read_only) {
  if (is_open()) return C3DB_FILE_ALREADY_OPEN_ERR;
  if (!base_file_name || base_file_name[0] == '\0') return C3DB_FILE_BAD_NAME_ERR;

  c3db_err_t err = irf_.begin(base_file_name, read_only, C3DB_INDEX_REF_EXTENSION);
  if (IS_ERR(err)) return err;

  size_t count = 0;
  c3db_err_t count_err = irf_.max_rec_count(count);
  /*
   * IRF can remain usable with a DBF free-list repair warning, but record 0
   * must exist because it stores the IRF metadata node.
   */
  if (IS_ERR(count_err)) err = count_err;
  else if (OK(err)) err = count_err;

  if ((OK(err) || IS_WNG(err)) && count == 0) err = C3DB_FILE_CORRUPT_ERR;

  c3db_ref_node_t hdr = {};
  if (OK(err) || IS_WNG(err)) {
    c3db_err_t hdr_err = read_hdr(hdr);
    if (IS_ERR(hdr_err)) err = hdr_err;
    else if (OK(err)) err = hdr_err;
  }

  if (OK(err) || IS_WNG(err)) {
    if (hdr.next_ref == 0) err = C3DB_FILE_CORRUPT_ERR;
  }

  if (IS_ERR(err)) {
    irf_.end();
    reset_state();
    return err;
  }

  first_free_ref_ = hdr.next_ref;
  read_only_ = read_only;
  return err;
}

c3db_err_t c3db_index_ref_t::end() {
  c3db_err_t err = irf_.end();
  reset_state();
  return err;
}

c3db_err_t c3db_index_ref_t::create_new_ref(c3db_id_t record_id, c3db_id_t next_ref, c3db_id_t &ref_id) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (record_id == C3DB_NULL_ID) return C3DB_INVALID_ARG_ERR;
  if (next_ref == 0) return C3DB_INVALID_ARG_ERR;

  c3db_ref_node_t node = {
    .record_id = record_id,
    .next_ref = next_ref
  };

  if (first_free_ref_ != C3DB_NULL_ID) {
    /*
     * Logical free nodes are not removed from DBF. The free-list head stores
     * the next reusable node in next_ref, then gets overwritten in one write
     * as an active reference node with its final list linkage.
     */
    ref_id = first_free_ref_;
    c3db_ref_node_t free_node = {};
    ON_ERR_RETURN(read_ref(ref_id, free_node));
    first_free_ref_ = free_node.next_ref;
    ON_ERR_RETURN(write_ref(ref_id, node));
    return persist_first_free_ref();
  }

  ON_ERR_RETURN(irf_.insert(reinterpret_cast<const uint8_t*>(&node), ref_id));
  if (ref_id == 0) return C3DB_FILE_CORRUPT_ERR;
  return C3DB_OK;
}

c3db_err_t c3db_index_ref_t::read_ref(c3db_id_t ref_id, c3db_ref_node_t &node) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (ref_id == C3DB_NULL_ID || ref_id == 0) return C3DB_INVALID_ARG_ERR;

  return irf_.select(ref_id, reinterpret_cast<uint8_t*>(&node));
}

c3db_err_t c3db_index_ref_t::write_ref(c3db_id_t ref_id, const c3db_ref_node_t &node) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (ref_id == C3DB_NULL_ID || ref_id == 0) return C3DB_INVALID_ARG_ERR;

  return irf_.update(ref_id, reinterpret_cast<const uint8_t*>(&node));
}

c3db_err_t c3db_index_ref_t::free_ref(c3db_id_t prev_ref, c3db_id_t ref_id, c3db_id_t &next_ref) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (ref_id == C3DB_NULL_ID || ref_id == 0) return C3DB_INVALID_ARG_ERR;
  if (prev_ref == C3DB_NULL_ID || prev_ref == 0 || prev_ref == ref_id) return C3DB_INVALID_ARG_ERR;

  c3db_ref_node_t node = {};
  ON_ERR_RETURN(read_ref(ref_id, node));
  next_ref = node.next_ref;

  c3db_ref_node_t prev_node = {};
  ON_ERR_RETURN(read_ref(prev_ref, prev_node));
  if (prev_node.next_ref != ref_id) return C3DB_INVALID_ARG_ERR;

  /*
   * The previous node is updated before releasing ref_id. A reset between both
   * writes can leak one IRF node, but it cannot leave the visible chain pointing
   * to a node already available for reuse.
   */
  prev_node.next_ref = next_ref;
  ON_ERR_RETURN(write_ref(prev_ref, prev_node));

  return irf_.remove(ref_id);
}

c3db_err_t c3db_index_ref_t::free_ref(c3db_id_t ref_id, c3db_id_t &next_ref) {
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (ref_id == C3DB_NULL_ID || ref_id == 0) return C3DB_INVALID_ARG_ERR;

  c3db_ref_node_t node = {};
  ON_ERR_RETURN(read_ref(ref_id, node));
  next_ref = node.next_ref;

  return irf_.remove(ref_id);
}

c3db_err_t c3db_index_ref_t::free_full_chain(c3db_id_t first_ref, c3db_id_t last_ref) {
  if (first_ref == C3DB_NULL_ID && last_ref == C3DB_NULL_ID) return C3DB_OK;
  if (!is_open()) return C3DB_FILE_NOT_OPEN_ERR;
  if (read_only_) return C3DB_READ_ONLY_ERR;
  if (first_ref == C3DB_NULL_ID || first_ref == 0 ||
      last_ref == C3DB_NULL_ID || last_ref == 0) {
    return C3DB_INVALID_ARG_ERR;
  }

  c3db_ref_node_t last_node = {};
  ON_ERR_RETURN(read_ref(last_ref, last_node));

  /*
   * The caller has already traversed the chain and knows its tail. Linking the
   * tail to the current free-list avoids a second O(n) pass over IRF.
   */
  last_node.next_ref = first_free_ref_;
  ON_ERR_RETURN(write_ref(last_ref, last_node));
  first_free_ref_ = first_ref;
  return persist_first_free_ref();
}

bool c3db_index_ref_t::is_open() const {
  return irf_.is_open();
}

bool c3db_index_ref_t::is_read_only() const {
  return read_only_;
}

c3db_err_t c3db_index_ref_t::read_hdr(c3db_ref_node_t &hdr) {
  c3db_db_rec_t rec(c3db_shared_buffer, sizeof(c3db_ref_node_t));
  ON_ERR_RETURN(rec.read_rec(&irf_.file(), 0));
  ON_ERR_RETURN(rec.parse());
  if (!rec.is_active()) return C3DB_REC_CORRUPT_ERR;
  return rec.get_payload(reinterpret_cast<uint8_t*>(&hdr));
}

c3db_err_t c3db_index_ref_t::write_hdr(const c3db_ref_node_t &hdr) {
  return irf_.update(c3db_id(0, 0), reinterpret_cast<const uint8_t*>(&hdr));
}

c3db_err_t c3db_index_ref_t::persist_first_free_ref() {
  c3db_ref_node_t hdr = {};
  hdr.record_id = C3DB_NULL_ID;
  hdr.next_ref = first_free_ref_;
  return write_hdr(hdr);
}

void c3db_index_ref_t::reset_state() {
  first_free_ref_ = C3DB_NULL_ID;
  read_only_ = false;
}

c3db_err_t c3db_index_ref_t::delete_ref_file(const char* base_file_name) const {
  char* irf_name = nullptr;
  c3db_err_t err = c3db_make_file_name(base_file_name, C3DB_INDEX_REF_EXTENSION, irf_name);
  if (OK(err) && c3db_file_t::exists(irf_name)) err = c3db_file_t::delete_file(irf_name);
  delete[] irf_name;
  return err;
}
