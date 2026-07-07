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
#include <stdio.h>

#include "esp_timer.h"

#include "c3db_benchmarks.h"
#include "c3db_cached_db_file_tests.h"
#include "c3db_data_file_tests.h"
#include "c3db_db_tests.h"
#include "c3db_db_file_tests.h"
#include "c3db_index_bucket_tests.h"
#include "c3db_index_tests.h"
#include "c3db_index_ref_tests.h"
#include "c3db_manual_tests.h"
#include "c3db_utils.h"
[[maybe_unused]] static void run_c3db_crc_benchmark() {
  printf("\nC3DB CRC32 benchmark start\n");

  static uint8_t buffer[4096];
  for (size_t i = 0; i < sizeof(buffer); ++i) {
    buffer[i] = static_cast<uint8_t>((i * 31u) + 7u);
  }

  struct crc_case_t {
    size_t size;
    size_t iterations;
  };

  const crc_case_t cases[] = {
    {1024, 5000},
    {2048, 3000},
    {4096, 1500}
  };

  volatile uint32_t sink = 0;
  for (const auto &test : cases) {
    const int64_t start_us = esp_timer_get_time();
    for (size_t i = 0; i < test.iterations; ++i) {
      sink ^= c3db_crc32(buffer, test.size);
    }
    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const double per_crc_ms = static_cast<double>(elapsed_us) /
                              static_cast<double>(test.iterations) /
                              1000.0;

    printf("%u bytes: %.6f ms per CRC (%u iterations)\n",
           static_cast<unsigned>(test.size),
           per_crc_ms,
           static_cast<unsigned>(test.iterations));
  }

  printf("CRC sink: %lu\n", static_cast<unsigned long>(sink));
  printf("C3DB CRC32 benchmark end\n");
}

void run_c3db_manual_tests() {
  printf("\nC3DB manual tests start\n");

  /*
   * Benchmarks are kept in components/c3db/benchmarks as TFG reference tools.
   * Enable only one call at a time when collecting metrics:
   *
   * - run_c3db_file_select_benchmark()
   * - run_c3db_db_file_select_benchmark()
   * - run_c3db_data_file_select_benchmark()
   * - run_c3db_db_file_append_benchmark()
   * - run_c3db_data_file_append_benchmark()
   * - run_c3db_db_file_update_benchmark()
   * - run_c3db_data_file_update_benchmark()
   * - run_c3db_db_file_remove_benchmark()
   * - run_c3db_data_file_remove_benchmark()
   * - run_c3db_db_file_insert_reuse_benchmark()
   * - run_c3db_data_file_insert_reuse_benchmark()
   * - run_c3db_index_new_hash_benchmark()
   * - run_c3db_index_existing_ref_benchmark()
   * - run_c3db_index_find_first_benchmark()
   */

  if (!run_c3db_db_file_tests()) {
    printf("C3DB manual tests FAILED\n");
    return;
  }

  if (!run_c3db_cached_db_file_tests()) {
    printf("C3DB manual tests FAILED\n");
    return;
  }

  if (!run_c3db_data_file_tests()) {
    printf("C3DB manual tests FAILED\n");
    return;
  }

  if (!run_c3db_index_bucket_tests()) {
    printf("C3DB manual tests FAILED\n");
    return;
  }

  if (!run_c3db_index_ref_tests()) {
    printf("C3DB manual tests FAILED\n");
    return;
  }

  if (!run_c3db_index_tests()) {
    printf("C3DB manual tests FAILED\n");
    return;
  }

  if (!run_c3db_db_tests()) {
    printf("C3DB manual tests FAILED\n");
    return;
  }

  printf("\nC3DB manual tests OK\n");
}
