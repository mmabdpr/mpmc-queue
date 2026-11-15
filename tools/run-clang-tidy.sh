#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
# SPDX-License-Identifier: CPAL-1.0
#
# The contents of this file are subject to the Common Public Attribution
# License Version 1.0 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# https://opensource.org/license/cpal-1-0.
# The License is based on the Mozilla Public License Version 1.1 but
# Sections 14 and 15 have been added to cover use of software over a
# computer network and provide for limited attribution for the Original
# Developer. In addition, Exhibit A has been modified to be consistent
# with Exhibit B.
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
# License for the specific language governing rights and limitations
# under the License.
#
# The Original Code is mpmc_queue, a header-only C++20 bounded queue
# library.
# The Original Developer is not separately identified and is the Initial
# Developer. If left blank, the Original Developer is the Initial
# Developer.
# The Initial Developer of the Original Code is Mohammad Mahdi
# Abdollahpour. All portions of the code written by Mohammad Mahdi
# Abdollahpour are Copyright (c) 2026. All Rights Reserved.
# Contributor: Mohammad Mahdi Abdollahpour.
#
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${MPMC_BUILD_DIR:-${root_dir}/build}"
clang_tidy="${CLANG_TIDY:-clang-tidy}"

if ! command -v "${clang_tidy}" >/dev/null 2>&1; then
    printf 'error: clang-tidy not found; set CLANG_TIDY to the executable\n' >&2
    exit 2
fi

compile_commands="${build_dir}/compile_commands.json"
if [[ ! -f "${compile_commands}" ]]; then
    cmake -S "${root_dir}" -B "${build_dir}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

if [[ ! -f "${compile_commands}" ]]; then
    printf 'error: %s was not generated\n' "${compile_commands}" >&2
    exit 2
fi

files=()
while IFS= read -r file; do
    files+=("${file}")
done < <(
    python3 - "${compile_commands}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    database = json.load(stream)

seen = set()
for entry in database:
    path = entry["file"]
    if path not in seen:
        seen.add(path)
        print(path)
PY
)

status=0
for file in "${files[@]}"; do
    printf 'checking %s\n' "${file#"${root_dir}"/}"
    if ! "${clang_tidy}" -p "${build_dir}" --warnings-as-errors='*' \
        "${file}"; then
        status=1
    fi
done

exit "${status}"
