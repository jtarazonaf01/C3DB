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

#include "c3db_index_ref_tests.h"

#include <stdio.h>

#include "c3db_index_ref.h"

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

static bool expect_err(const char* label, c3db_err_t expected, c3db_err_t got) {
  if (got == expected) return true;
  printf("%s failed: expected=%d got=%d\n", label, expected, got);
  return false;
}

static bool expect_true(const char* label, bool value) {
  if (value) return true;
  printf("%s failed\n", label);
  return false;
}

static bool run_c3db_index_ref_basic_test() {
  printf("c3db_index_ref_t basic test start\n");

  const char* base_name = "/sdcard/irfb";
  const char* irf_name = "/sdcard/irfb.irf";

  remove(irf_name);

  c3db_index_ref_t refs;
  if (!expect_ok("irf create", refs.create(base_name))) return false;
  if (!expect_true("irf open", refs.is_open())) return false;
  if (!expect_true("irf writable", !refs.is_read_only())) return false;

  c3db_id_t ref0 = C3DB_NULL_ID;
  c3db_id_t ref1 = C3DB_NULL_ID;
  if (!expect_ok("irf create ref 0", refs.create_new_ref(0x100, C3DB_NULL_ID, ref0))) return false;
  if (!expect_ok("irf create ref 1", refs.create_new_ref(0x101, C3DB_NULL_ID, ref1))) return false;
  if (!expect_true("irf ref 0 row", test_row(ref0) == 1)) return false;
  if (!expect_true("irf ref 1 row", test_row(ref1) == 2)) return false;
  if (!expect_true("irf ref cycles", test_cycle(ref0) == 0 && test_cycle(ref1) == 0)) return false;

  c3db_ref_node_t node0 = {};
  c3db_ref_node_t node1 = {};
  if (!expect_ok("irf read ref 0", refs.read_ref(ref0, node0))) return false;
  if (!expect_ok("irf read ref 1", refs.read_ref(ref1, node1))) return false;
  if (!expect_true("irf ref 0 payload", node0.record_id == 0x100 && node0.next_ref == C3DB_NULL_ID)) return false;
  if (!expect_true("irf ref 1 payload", node1.record_id == 0x101 && node1.next_ref == C3DB_NULL_ID)) return false;

  node0.next_ref = ref1;
  if (!expect_ok("irf write ref 0", refs.write_ref(ref0, node0))) return false;
  if (!expect_ok("irf reread ref 0", refs.read_ref(ref0, node0))) return false;
  if (!expect_true("irf linked ref", node0.next_ref == ref1)) return false;
  if (!expect_err("irf reject meta read", C3DB_INVALID_ARG_ERR, refs.read_ref(0, node0))) return false;
  if (!expect_ok("irf close", refs.end())) return false;

  c3db_index_ref_t reopened;
  if (!expect_ok("irf reopen", reopened.begin(base_name))) return false;
  if (!expect_ok("irf reopened read", reopened.read_ref(ref0, node0))) return false;
  if (!expect_true("irf reopened payload", node0.record_id == 0x100 && node0.next_ref == ref1)) return false;
  if (!expect_ok("irf reopened close", reopened.end())) return false;

  printf("c3db_index_ref_t basic test OK\n");
  return true;
}

static bool run_c3db_index_ref_free_list_test() {
  printf("c3db_index_ref_t free-list test start\n");

  const char* base_name = "/sdcard/irff";
  const char* irf_name = "/sdcard/irff.irf";

  remove(irf_name);

  c3db_index_ref_t refs;
  if (!expect_ok("irf free create", refs.create(base_name))) return false;

  c3db_id_t ref0 = C3DB_NULL_ID;
  c3db_id_t ref1 = C3DB_NULL_ID;
  c3db_id_t ref2 = C3DB_NULL_ID;
  if (!expect_ok("irf free create 0", refs.create_new_ref(0x200, C3DB_NULL_ID, ref0))) return false;
  if (!expect_ok("irf free create 1", refs.create_new_ref(0x201, C3DB_NULL_ID, ref1))) return false;
  if (!expect_ok("irf free create 2", refs.create_new_ref(0x202, C3DB_NULL_ID, ref2))) return false;

  c3db_ref_node_t node0 = {};
  c3db_ref_node_t node1 = {};
  if (!expect_ok("irf free read 0", refs.read_ref(ref0, node0))) return false;
  if (!expect_ok("irf free read 1", refs.read_ref(ref1, node1))) return false;
  node0.next_ref = ref1;
  node1.next_ref = ref2;
  if (!expect_ok("irf free link 0", refs.write_ref(ref0, node0))) return false;
  if (!expect_ok("irf free link 1", refs.write_ref(ref1, node1))) return false;

  if (!expect_ok("irf free chain", refs.free_full_chain(ref0, ref2))) return false;

  c3db_id_t reused0 = C3DB_NULL_ID;
  c3db_id_t reused1 = C3DB_NULL_ID;
  c3db_id_t reused2 = C3DB_NULL_ID;
  if (!expect_ok("irf reuse 0", refs.create_new_ref(0x300, C3DB_NULL_ID, reused0))) return false;
  if (!expect_ok("irf reuse 1", refs.create_new_ref(0x301, C3DB_NULL_ID, reused1))) return false;
  if (!expect_ok("irf reuse 2", refs.create_new_ref(0x302, C3DB_NULL_ID, reused2))) return false;

  if (!expect_true("irf reuse first row", test_row(reused0) == test_row(ref0))) return false;
  if (!expect_true("irf reuse second row", test_row(reused1) == test_row(ref1))) return false;
  if (!expect_true("irf reuse third row", test_row(reused2) == test_row(ref2))) return false;

  c3db_ref_node_t out = {};
  if (!expect_ok("irf reuse read 0", refs.read_ref(reused0, out))) return false;
  if (!expect_true("irf reuse payload 0", out.record_id == 0x300 && out.next_ref == C3DB_NULL_ID)) return false;
  if (!expect_ok("irf free close", refs.end())) return false;

  c3db_index_ref_t reopened;
  if (!expect_ok("irf free reopen", reopened.begin(base_name))) return false;

  c3db_id_t ref3 = C3DB_NULL_ID;
  if (!expect_ok("irf free append after empty logical free-list", reopened.create_new_ref(0x400, C3DB_NULL_ID, ref3))) return false;
  if (!expect_true("irf free new row after empty", test_row(ref3) == 4)) return false;
  if (!expect_ok("irf free reopened close", reopened.end())) return false;

  printf("c3db_index_ref_t free-list test OK\n");
  return true;
}

static bool run_c3db_index_ref_free_ref_test() {
  printf("c3db_index_ref_t free ref test start\n");

  const char* base_name = "/sdcard/irfu";
  const char* irf_name = "/sdcard/irfu.irf";

  remove(irf_name);

  c3db_index_ref_t refs;
  if (!expect_ok("irf free ref create", refs.create(base_name))) return false;

  c3db_id_t ref0 = C3DB_NULL_ID;
  c3db_id_t ref1 = C3DB_NULL_ID;
  c3db_id_t ref2 = C3DB_NULL_ID;
  if (!expect_ok("irf free ref create 0", refs.create_new_ref(0x500, C3DB_NULL_ID, ref0))) return false;
  if (!expect_ok("irf free ref create 1", refs.create_new_ref(0x501, C3DB_NULL_ID, ref1))) return false;
  if (!expect_ok("irf free ref create 2", refs.create_new_ref(0x502, C3DB_NULL_ID, ref2))) return false;

  c3db_ref_node_t node0 = {};
  c3db_ref_node_t node1 = {};
  if (!expect_ok("irf free ref read 0", refs.read_ref(ref0, node0))) return false;
  if (!expect_ok("irf free ref read 1", refs.read_ref(ref1, node1))) return false;
  node0.next_ref = ref1;
  node1.next_ref = ref2;
  if (!expect_ok("irf free ref link 0", refs.write_ref(ref0, node0))) return false;
  if (!expect_ok("irf free ref link 1", refs.write_ref(ref1, node1))) return false;

  c3db_id_t next_ref = C3DB_NULL_ID;
  if (!expect_err("irf free ref reject head", C3DB_INVALID_ARG_ERR,
                  refs.free_ref(C3DB_NULL_ID, ref0, next_ref))) return false;

  if (!expect_ok("irf free ref middle", refs.free_ref(ref0, ref1, next_ref))) return false;
  if (!expect_true("irf free ref next", next_ref == ref2)) return false;
  if (!expect_ok("irf free ref reread 0", refs.read_ref(ref0, node0))) return false;
  if (!expect_true("irf free ref chain patched", node0.next_ref == ref2)) return false;

  c3db_id_t reused = C3DB_NULL_ID;
  if (!expect_ok("irf free ref reuse", refs.create_new_ref(0x600, C3DB_NULL_ID, reused))) return false;
  if (!expect_true("irf free ref reused row", test_row(reused) == test_row(ref1))) return false;
  if (!expect_true("irf free ref reused cycle", test_cycle(reused) == test_cycle(ref1) + 1)) return false;
  if (!expect_ok("irf free ref close", refs.end())) return false;

  printf("c3db_index_ref_t free ref test OK\n");
  return true;
}

bool run_c3db_index_ref_tests() {
  if (!run_c3db_index_ref_basic_test()) return false;
  if (!run_c3db_index_ref_free_list_test()) return false;
  if (!run_c3db_index_ref_free_ref_test()) return false;
  return true;
}
