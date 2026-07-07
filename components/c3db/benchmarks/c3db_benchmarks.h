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

/**
 * Low-level random-read baseline using fseek() + fread().
 */
bool run_c3db_low_level_random_read_benchmark();

/**
 * Low-level write baseline where each write extends a newly created file.
 */
bool run_c3db_low_level_extend_write_benchmark();

/**
 * Low-level write baseline where each write overwrites an existing file.
 */
bool run_c3db_low_level_overwrite_benchmark();

/**
 * c3db_file_t fixed-record select() benchmark.
 */
bool run_c3db_file_select_benchmark();

/**
 * c3db_file_t fixed-record append() benchmark.
 */
bool run_c3db_file_append_benchmark();

/**
 * c3db_file_t fixed-record update() benchmark.
 */
bool run_c3db_file_update_benchmark();

/**
 * c3db_db_file_t logical-payload select() benchmark.
 */
bool run_c3db_db_file_select_benchmark();

/**
 * c3db_data_file_t logical-payload select() benchmark.
 */
bool run_c3db_data_file_select_benchmark();

/**
 * c3db_db_file_t logical-payload append() benchmark.
 */
bool run_c3db_db_file_append_benchmark();

/**
 * c3db_data_file_t logical-payload append() benchmark.
 */
bool run_c3db_data_file_append_benchmark();

/**
 * c3db_db_file_t logical-payload update() benchmark.
 */
bool run_c3db_db_file_update_benchmark();

/**
 * c3db_data_file_t logical-payload update() benchmark.
 */
bool run_c3db_data_file_update_benchmark();

/**
 * c3db_db_file_t remove() benchmark.
 */
bool run_c3db_db_file_remove_benchmark();

/**
 * c3db_data_file_t remove() benchmark.
 */
bool run_c3db_data_file_remove_benchmark();

/**
 * c3db_db_file_t insert() benchmark reusing deleted records.
 */
bool run_c3db_db_file_insert_reuse_benchmark();

/**
 * c3db_data_file_t insert() benchmark reusing deleted records.
 */
bool run_c3db_data_file_insert_reuse_benchmark();

/**
 * c3db_index_t benchmark for indexing hashes that are not yet present.
 */
bool run_c3db_index_new_hash_benchmark();

/**
 * c3db_index_t benchmark for adding references to existing hashes.
 */
bool run_c3db_index_existing_ref_benchmark();

/**
 * c3db_index_t benchmark for finding the first reference of indexed hashes.
 */
bool run_c3db_index_find_first_benchmark();
