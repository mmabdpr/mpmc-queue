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
// Error codes used by the fallible setup paths.  The queue hot path never
// reports errors: it returns false on full/empty/exhausted conditions.

#pragma once

#include <string>
#include <system_error>
#include <type_traits>

namespace mpmc {

enum class errc {
    invalid_capacity = 1,
    invalid_name,
    name_too_long,
    allocation_failed,
    already_exists,
    not_found,
    not_ready,
    mapping_too_small,
    incompatible_layout,
};

[[nodiscard]] inline const std::error_category& error_category() noexcept {
    class category final : public std::error_category {
    public:
        [[nodiscard]] const char* name() const noexcept override {
            return "mpmc";
        }

        [[nodiscard]] std::string message(int value) const override {
            switch (static_cast<errc>(value)) {
            case errc::invalid_capacity:
                return "invalid queue capacity";
            case errc::invalid_name:
                return "invalid shared-memory name";
            case errc::name_too_long:
                return "shared-memory name is too long";
            case errc::allocation_failed:
                return "allocation failed";
            case errc::already_exists:
                return "shared-memory name already exists";
            case errc::not_found:
                return "shared-memory name does not exist";
            case errc::not_ready:
                return "shared-memory queue is not initialized";
            case errc::mapping_too_small:
                return "shared-memory object is too small";
            case errc::incompatible_layout:
                return "shared-memory queue layout is incompatible";
            }
            return "unknown mpmc error";
        }
    };

    static const category instance;
    return instance;
}

[[nodiscard]] inline std::error_code make_error_code(errc value) noexcept {
    return {static_cast<int>(value), error_category()};
}

} // namespace mpmc

namespace std {
template<> struct is_error_code_enum<mpmc::errc> : true_type {};
} // namespace std
