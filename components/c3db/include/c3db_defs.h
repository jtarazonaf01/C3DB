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

#include "c3db_types.h"

//! Returns true when a C3DB error code represents success.
#define OK(err) ((err) == C3DB_OK)

//! Returns true when a C3DB code represents any non-success result.
#define KO(err) ((err) != C3DB_OK)

//! Returns true when a C3DB code represents an error.
#define IS_ERR(err) ((err) > C3DB_OK)

//! Returns true when a C3DB code represents a warning.
#define IS_WNG(err) ((err) < C3DB_OK)

//! Returns immediately when expr evaluates to an error.
#define ON_ERR_RETURN(expr)           \
  do {                                \
    c3db_err_t _c3db_err = (expr);    \
    if (IS_ERR(_c3db_err)) return _c3db_err; \
  } while (0)

//! Returns immediately when expr evaluates to a warning.
#define ON_WNG_RETURN(expr)           \
  do {                                \
    c3db_err_t _c3db_err = (expr);    \
    if (IS_WNG(_c3db_err)) return _c3db_err; \
  } while (0)

//! Returns immediately when expr does not evaluate to C3DB_OK.
#define ON_KO_RETURN(expr)            \
  do {                                \
    c3db_err_t _c3db_err = (expr);    \
    if (KO(_c3db_err)) return _c3db_err; \
  } while (0)
