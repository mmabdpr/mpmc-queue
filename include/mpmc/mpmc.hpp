// SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
// SPDX-License-Identifier: CPAL-1.0
//
// The contents of this file are subject to the Common Public Attribution
// License Version 1.0 (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// https://opensource.org/license/cpal-1-0.
// The License is based on the Mozilla Public License Version 1.1 but
// Sections 14 and 15 have been added to cover use of software over a
// computer network and provide for limited attribution for the Original
// Developer. In addition, Exhibit A has been modified to be consistent
// with Exhibit B.
//
// Software distributed under the License is distributed on an "AS IS"
// basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
// License for the specific language governing rights and limitations
// under the License.
//
// The Original Code is mpmc_queue, a header-only C++20 bounded queue
// library.
// The Original Developer is not separately identified and is the Initial
// Developer. If left blank, the Original Developer is the Initial
// Developer.
// The Initial Developer of the Original Code is Mohammad Mahdi
// Abdollahpour. All portions of the code written by Mohammad Mahdi
// Abdollahpour are Copyright (c) 2026. All Rights Reserved.
// Contributor: Mohammad Mahdi Abdollahpour.
//
// Umbrella header for the mpmc_queue library.

#pragma once

#include "mpmc/concepts.hpp"
#include "mpmc/concurrency.hpp"
#include "mpmc/error.hpp"
#include "mpmc/expected.hpp"
#include "mpmc/mpmc_queue.hpp"
#include "mpmc/shared_memory_queue.hpp"
#include "mpmc/spsc_queue.hpp"
#include "mpmc/unique_ptr_queue.hpp"
#include "mpmc/version.hpp"
