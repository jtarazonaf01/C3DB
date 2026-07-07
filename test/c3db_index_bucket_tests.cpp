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

#include "c3db_index_bucket_tests.h"

#include <cstring>
#include <stdio.h>

#include "c3db_index_bucket.h"

static uint32_t test_row(c3db_id_t id) {
  return static_cast<uint32_t>(id & 0xFFFFFFFFu);
}

static uint32_t test_cycle(c3db_id_t id) {
  return static_cast<uint32_t>(id >> 32);
}

static bool expect_ok(const char* label, c3db_err_t got) {
  if (got == C3DB_OK) return true;
  printf("%s failed: expected=%d got=%d\n", label, C3DB_OK, got);
  return false;
}

static bool expect_true(const char* label, bool value) {
  if (value) return true;
  printf("%s failed\n", label);
  return false;
}

static void fill_bucket(c3db_bucket_t &bucket, uint64_t hash_base, c3db_id_t ref_base, uint32_t count) {
  std::memset(&bucket, 0, sizeof(bucket));
  bucket.count = count;
  bucket.next_overflow = C3DB_NULL_ID;

  for (uint32_t i = 0; i < count; ++i) {
    bucket.entries[i].hash = hash_base + i;
    bucket.entries[i].first_ref = ref_base + i;
  }
}

static bool bucket_prefix_eq(const c3db_bucket_t &a, const c3db_bucket_t &b) {
  if (a.count != b.count || a.next_overflow != b.next_overflow) return false;

  for (uint32_t i = 0; i < a.count; ++i) {
    if (a.entries[i].hash != b.entries[i].hash ||
        a.entries[i].first_ref != b.entries[i].first_ref) {
      return false;
    }
  }

  return true;
}

static bool run_c3db_index_bucket_basic_test() {
  printf("c3db_index_bucket_t basic test start\n");

  const char* base_name = "/sdcard/ibasic";
  const char* icb_name = "/sdcard/ibasic.icb";
  const char* iob_name = "/sdcard/ibasic.iob";

  remove(icb_name);
  remove(iob_name);

  c3db_index_bucket_t buckets;
  if (!expect_ok("index bucket create", buckets.create(base_name))) return false;
  if (!expect_true("index bucket is open", buckets.is_open())) return false;
  if (!expect_true("index bucket is writable", !buckets.is_read_only())) return false;

  static c3db_bucket_t bucket0;
  std::memset(&bucket0, 0, sizeof(bucket0));
  if (!expect_ok("index bucket read canonical 0", buckets.read_canonical(0, bucket0))) return false;
  if (!expect_true("index bucket canonical 0 empty", bucket0.count == 0)) return false;
  if (!expect_true("index bucket canonical 0 no overflow", bucket0.next_overflow == C3DB_NULL_ID)) return false;

  c3db_id_t bucket1_id = C3DB_NULL_ID;
  if (!expect_ok("index bucket append canonical", buckets.append_canonical_bucket(bucket1_id))) return false;
  if (!expect_true("index bucket canonical row", test_row(bucket1_id) == 1)) return false;
  if (!expect_true("index bucket canonical cycle", test_cycle(bucket1_id) == 0)) return false;

  static c3db_bucket_t bucket1;
  fill_bucket(bucket1, 0x1000, 0x2000, 2);
  if (!expect_ok("index bucket write canonical", buckets.write_canonical(1, bucket1))) return false;

  static c3db_bucket_t out;
  std::memset(&out, 0, sizeof(out));
  if (!expect_ok("index bucket reread canonical", buckets.read_canonical(1, out))) return false;
  if (!expect_true("index bucket canonical payload", bucket_prefix_eq(out, bucket1))) return false;
  if (!expect_ok("index bucket close", buckets.end())) return false;

  c3db_index_bucket_t reopened;
  if (!expect_ok("index bucket reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("index bucket reopened read", reopened.read_canonical(1, out))) return false;
  if (!expect_true("index bucket reopened payload", bucket_prefix_eq(out, bucket1))) return false;
  if (!expect_ok("index bucket reopened close", reopened.end())) return false;

  printf("c3db_index_bucket_t basic test OK\n");
  return true;
}

static bool run_c3db_index_bucket_overflow_test() {
  printf("c3db_index_bucket_t overflow test start\n");

  const char* base_name = "/sdcard/iovf";
  const char* icb_name = "/sdcard/iovf.icb";
  const char* iob_name = "/sdcard/iovf.iob";

  remove(icb_name);
  remove(iob_name);

  c3db_index_bucket_t buckets;
  if (!expect_ok("index ovf create", buckets.create(base_name))) return false;

  static c3db_bucket_t second;
  fill_bucket(second, 0x3000, 0x4000, 1);

  c3db_id_t second_id = C3DB_NULL_ID;
  if (!expect_ok("index ovf create second", buckets.create_overflow_bkt(second, second_id))) return false;

  static c3db_bucket_t first;
  fill_bucket(first, 0x5000, 0x6000, 2);
  first.next_overflow = second_id;

  c3db_id_t first_id = C3DB_NULL_ID;
  if (!expect_ok("index ovf create first", buckets.create_overflow_bkt(first, first_id))) return false;

  static c3db_bucket_t out;
  std::memset(&out, 0, sizeof(out));
  if (!expect_ok("index ovf read first", buckets.read_overflow_bkt(first_id, out))) return false;
  if (!expect_true("index ovf first payload", bucket_prefix_eq(out, first))) return false;

  static c3db_bucket_t updated;
  fill_bucket(updated, 0x7000, 0x8000, 3);
  updated.next_overflow = second_id;
  if (!expect_ok("index ovf write first", buckets.write_overflow_bkt(first_id, updated))) return false;
  if (!expect_ok("index ovf reread first", buckets.read_overflow_bkt(first_id, out))) return false;
  if (!expect_true("index ovf updated payload", bucket_prefix_eq(out, updated))) return false;

  /*
   * Freeing a chain should make its IOB records available to DBF insert().
   * The exact reused row is implementation-dependent because the DBF free-list
   * is stack-like, so the test checks reuse of either freed physical row.
   */
  if (!expect_ok("index ovf free chain", buckets.free_overflow_bkt_chain(first_id))) return false;

  static c3db_bucket_t reused;
  fill_bucket(reused, 0x9000, 0xA000, 1);

  c3db_id_t reused_id = C3DB_NULL_ID;
  if (!expect_ok("index ovf create reused", buckets.create_overflow_bkt(reused, reused_id))) return false;

  const bool reused_row = test_row(reused_id) == test_row(first_id) ||
                          test_row(reused_id) == test_row(second_id);
  if (!expect_true("index ovf reused freed row", reused_row)) return false;
  if (!expect_true("index ovf reused advanced cycle", test_cycle(reused_id) == 1)) return false;
  if (!expect_ok("index ovf close", buckets.end())) return false;

  printf("c3db_index_bucket_t overflow test OK\n");
  return true;
}

static bool run_c3db_index_bucket_link_overflow_test() {
  printf("c3db_index_bucket_t link overflow test start\n");

  const char* base_name = "/sdcard/ilink";
  const char* icb_name = "/sdcard/ilink.icb";
  const char* iob_name = "/sdcard/ilink.iob";

  remove(icb_name);
  remove(iob_name);

  c3db_index_bucket_t buckets;
  if (!expect_ok("index link create", buckets.create(base_name))) return false;

  static c3db_bucket_t first;
  fill_bucket(first, 0x1100, 0x2100, 1);

  c3db_id_t first_id = C3DB_NULL_ID;
  const uint32_t canonical_num = 0;
  if (!expect_ok("index link from canonical", buckets.link_from_canonical_bkt(canonical_num, first, first_id))) return false;

  static c3db_bucket_t canonical;
  if (!expect_ok("index link read canonical", buckets.read_canonical(canonical_num, canonical))) return false;
  if (!expect_true("index link canonical next", canonical.next_overflow == first_id)) return false;

  static c3db_bucket_t out;
  if (!expect_ok("index link read first", buckets.read_overflow_bkt(first_id, out))) return false;
  if (!expect_true("index link first payload", bucket_prefix_eq(out, first))) return false;

  static c3db_bucket_t second;
  fill_bucket(second, 0x1200, 0x2200, 2);

  c3db_id_t second_id = C3DB_NULL_ID;
  if (!expect_ok("index link from overflow", buckets.link_from_overflow_bkt(first_id, second, second_id))) return false;

  if (!expect_ok("index link reread first", buckets.read_overflow_bkt(first_id, out))) return false;
  if (!expect_true("index link first next", out.next_overflow == second_id)) return false;
  if (!expect_ok("index link read second", buckets.read_overflow_bkt(second_id, out))) return false;
  if (!expect_true("index link second payload", bucket_prefix_eq(out, second))) return false;
  if (!expect_ok("index link close", buckets.end())) return false;

  printf("c3db_index_bucket_t link overflow test OK\n");
  return true;
}

bool run_c3db_index_bucket_tests() {
  if (!run_c3db_index_bucket_basic_test()) return false;
  if (!run_c3db_index_bucket_overflow_test()) return false;
  if (!run_c3db_index_bucket_link_overflow_test()) return false;
  return true;
}
