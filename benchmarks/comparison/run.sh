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

export PYTHONDONTWRITEBYTECODE=1

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
deps="$here/deps"
build="$here/build"
binary="$build/benchmarks/comparison/mpmc_comparison"
spsc_binary="$build/benchmarks/comparison/mpmc_spsc_comparison"

# Keep the host awake and use a latency-oriented scheduler tier on macOS.
# These settings apply equally to every library in the profile.
runner=()
if command -v caffeinate >/dev/null 2>&1; then
    runner+=(caffeinate -dimsu)
fi
if [ "$(uname -s)" = "Darwin" ] && command -v taskpolicy >/dev/null 2>&1; then
    runner+=(taskpolicy -a -t 1 -l 1)
    export MPMC_TASKPOLICY=1
fi

"$here/fetch_deps.sh" >&2

cmake -S "$root" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMPMC_BUILD_TESTS=OFF \
    -DMPMC_BUILD_EXAMPLES=OFF \
    -DMPMC_BUILD_BENCHMARKS=OFF \
    -DMPMC_BUILD_COMPARISON_BENCHMARKS=ON \
    -DMPMC_COMPARISON_DEPS_DIR="$deps" >&2
cmake --build "$build" --parallel 4 >&2

if [[ " $* " == *" --quick "* ]]; then
    exec "${runner[@]}" "$binary" "$@"
fi

current_load()
{
    if [ -r /proc/loadavg ]; then
        awk '{print $1}' /proc/loadavg
    else
        sysctl -n vm.loadavg | awk '{print $2}'
    fi
}

# The parallel build raises the one-minute load average.  Do not let that
# transient make the benchmark's own host gate reject an otherwise idle host.
if [[ " $* " != *" --allow-busy-host "* ]]; then
    attempt=0
    while [ "$attempt" -lt 40 ]; do
        load=$(current_load)
        if awk -v load="$load" 'BEGIN { exit !(load < 3.0) }'; then
            break
        fi
        printf 'waiting for host load to settle (load %.2f)\n' "$load" >&2
        sleep 15
        attempt=$((attempt + 1))
    done
    if [ "$attempt" -ge 40 ]; then
        echo "error: host load did not settle below 3.0" >&2
        exit 1
    fi
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/mpmc-comparison.XXXXXX")
staged_json=$(mktemp "$here/.reference.XXXXXX")
staged_md=$(mktemp "$here/.RESULTS.XXXXXX")
staged_spsc=$(mktemp "$here/.SPSC.XXXXXX")
cleanup()
{
    rm -rf "$tmpdir"
    rm -f "$staged_json" "$staged_md" "$staged_spsc"
}
trap cleanup EXIT

run_json="$tmpdir/trials.jsonl"
if ! "${runner[@]}" "$binary" "$@" > "$run_json"; then
    rm -f "$staged_json" "$staged_md" "$staged_spsc"
    trap - EXIT
    echo "error: benchmark execution failed" >&2
    echo "error: diagnostic JSON retained at $run_json" >&2
    exit 1
fi
if ! python3 "$here/render_results.py" \
        "$run_json" \
        "$staged_json" \
        "$staged_md"; then
    rm -f "$staged_json" "$staged_md" "$staged_spsc"
    trap - EXIT
    echo "error: reference run was not published" >&2
    echo "error: diagnostic JSON retained at $run_json" >&2
    exit 1
fi

# The topology-specialized 1P/1C profile is part of the same suite but is
# reported separately from the general MPMC ranking.  It uses strict FIFO
# validation and no per-item bitmap, so it measures the dedicated SPSC path.
spsc_json="$tmpdir/spsc.jsonl"
if ! "${runner[@]}" "$spsc_binary" > "$spsc_json"; then
    rm -f "$staged_json" "$staged_md" "$staged_spsc"
    trap - EXIT
    echo "error: SPSC benchmark execution failed" >&2
    echo "error: diagnostic JSON retained at $spsc_json" >&2
    exit 1
fi
if ! python3 "$here/spsc_render.py" "$spsc_json" "$staged_spsc"; then
    rm -f "$staged_json" "$staged_md" "$staged_spsc"
    trap - EXIT
    echo "error: SPSC result was not published" >&2
    exit 1
fi

# Stage in the comparison directory so publication is a same-filesystem rename.
chmod 0644 "$staged_json" "$staged_md" "$staged_spsc"
mv "$staged_json" "$here/reference.json"
mv "$staged_md" "$here/RESULTS.md"
mv "$staged_spsc" "$here/SPSC.md"
printf 'wrote %s, %s, and %s\n' "$here/reference.json" "$here/RESULTS.md" \
    "$here/SPSC.md"
cleanup
