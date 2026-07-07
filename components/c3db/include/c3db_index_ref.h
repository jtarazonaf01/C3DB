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

//! File extension used for index reference nodes.
static constexpr const char* C3DB_INDEX_REF_EXTENSION = ".irf";

/**
 * Persistent node in an index reference list.
 */
struct __attribute__((packed)) c3db_ref_node_t
{
  c3db_id_t record_id;
  c3db_id_t next_ref;
};

/**
 * Persistent storage manager for index reference lists.
 */
class c3db_index_ref_t
{
public:
  //! Creates an unopened IRF manager.
  c3db_index_ref_t();

  //! Creates the IRF file and reserves record 0 for metadata.
  c3db_err_t create(const char* base_file_name);

  //! Opens an existing IRF file and loads its metadata.
  c3db_err_t begin(const char* base_file_name, bool read_only = false);

  //! Closes the IRF file and clears transient state.
  c3db_err_t end();

  //! Creates a new reference node, reusing a logical free node when available.
  c3db_err_t create_new_ref(c3db_id_t record_id, c3db_id_t next_ref, c3db_id_t &ref_id);

  //! Reads a reference node. Record 0 is metadata and is not a valid ref node.
  c3db_err_t read_ref(c3db_id_t ref_id, c3db_ref_node_t &node);

  //! Writes an existing reference node.
  c3db_err_t write_ref(c3db_id_t ref_id, const c3db_ref_node_t &node);

  //! Frees a non-head reference node and returns the next node in the chain.
  c3db_err_t free_ref(c3db_id_t prev_ref, c3db_id_t ref_id, c3db_id_t &next_ref);

  //! Frees a reference node already detached from its visible chain.
  c3db_err_t free_ref(c3db_id_t ref_id, c3db_id_t &next_ref);

  //! Adds a fully detached reference chain to the logical IRF free-list.
  c3db_err_t free_full_chain(c3db_id_t first_ref, c3db_id_t last_ref);

  //! Returns true when the IRF file is open.
  bool is_open() const;

  //! Returns true when the IRF manager is opened read-only.
  bool is_read_only() const;

private:
  c3db_err_t read_hdr(c3db_ref_node_t &hdr);
  c3db_err_t write_hdr(const c3db_ref_node_t &hdr);
  c3db_err_t persist_first_free_ref();
  void reset_state();
  c3db_err_t delete_ref_file(const char* base_file_name) const;

  c3db_db_file_t irf_;
  c3db_id_t first_free_ref_;
  bool read_only_;
};
