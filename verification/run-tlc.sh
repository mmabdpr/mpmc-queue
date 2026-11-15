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
# Run the bounded TLA+ model with TLC.
#
# The pinned tools jar is cached under ${TMPDIR:-/tmp}, but its SHA-256 is
# verified on *every* invocation, not only after a download: a cached jar that
# no longer matches the pin is discarded and fetched again.  Downloads go to a
# unique temporary file and are renamed into place only after verification, so
# a failed or corrupt transfer never becomes the cached jar.
#
# TLC scratch state, fingerprint files and any counterexample trace are written
# under a temporary directory outside the repository.  A successful run removes
# it; a failing run keeps it and prints the path so the trace can be inspected.
# Set KEEP_TLC_ARTIFACTS=1 to keep a successful run's directory as well.
set -euo pipefail

root_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
version="1.8.0"
sha256="7e53aa636dac8a3cb0533f343115c0548e03fc0ce5edc7739becf58d3d0fb84f"
url="https://github.com/tlaplus/tlaplus/releases/download/v${version}/tla2tools.jar"
cache_dir="${TMPDIR:-/tmp}"
jar="${cache_dir%/}/mpmc-tla2tools-${version}.jar"

if ! command -v java >/dev/null 2>&1; then
    echo "error: java not found; install a Java 11+ runtime to run TLC" >&2
    exit 1
fi
if ! java -version >/dev/null 2>&1; then
    echo "error: java is on PATH but no runtime is usable; install a Java 11+ runtime" >&2
    exit 1
fi

# Returns success only when $1 has exactly the pinned checksum.  Output is
# suppressed because this also runs on every cached-jar invocation.
verify_sha256() {
    local file="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s  %s\n' "$sha256" "$file" | sha256sum -c - >/dev/null 2>&1
    elif command -v shasum >/dev/null 2>&1; then
        printf '%s  %s\n' "$sha256" "$file" | shasum -a 256 -c - >/dev/null 2>&1
    else
        echo "error: need sha256sum or shasum to verify the TLA+ tools jar" >&2
        return 1
    fi
}

if [[ -f "$jar" ]] && verify_sha256 "$jar"; then
    : # cached jar is intact
else
    if [[ -f "$jar" ]]; then
        echo "warning: cached $jar does not match the pinned SHA-256; discarding" >&2
        rm -f "$jar"
    fi
    tmp_jar="$(mktemp "${cache_dir%/}/mpmc-tla2tools-${version}.XXXXXX")"
    trap 'rm -f "$tmp_jar"' EXIT
    echo "==> downloading $url"
    curl -L --fail --silent --show-error "$url" -o "$tmp_jar"
    if ! verify_sha256 "$tmp_jar"; then
        echo "error: downloaded TLA+ tools jar does not match the pinned SHA-256" >&2
        exit 1
    fi
    mv -f "$tmp_jar" "$jar" # atomic rename within the cache directory
    trap - EXIT
fi

work_dir="$(mktemp -d "${cache_dir%/}/mpmc-tlc.XXXXXX")"
current_config=""

# A successful run is ephemeral, but a failing run keeps its scratch directory
# so the counterexample trace and TLC state can be inspected.  Set
# KEEP_TLC_ARTIFACTS=1 to keep a successful run's directory as well.
cleanup() {
    local status=$?
    local keep="${KEEP_TLC_ARTIFACTS:-0}"
    trap - EXIT INT HUP TERM
    if [[ $status -ne 0 ]]; then
        echo "error: TLC run failed${current_config:+ for $current_config}" \
             "(exit status $status)" >&2
        echo "       kept scratch directory: $work_dir" >&2
        echo "       counterexample trace (if any):" \
             "$work_dir/error-trace-${current_config:-*}.json" >&2
        echo "       TLC binary trace (if any):" \
             "$work_dir/MpmcQueue_TTrace_*.bin" >&2
        echo "       TLC state and fingerprints: $work_dir/meta-*" >&2
        echo "       remove it with: rm -rf '$work_dir'" >&2
    elif [[ "$keep" == "1" ]]; then
        echo "==> kept TLC scratch directory: $work_dir" \
             "(KEEP_TLC_ARTIFACTS=1)"
    else
        rm -rf -- "$work_dir" || true
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 129' HUP
trap 'exit 143' TERM

# TLC writes a counterexample trace next to the spec module, not into
# -metadir.  Run a copy of the module so even a failing run cannot create
# verification/MpmcQueue_TTrace_* in the repository.
cp "$root_dir/verification/MpmcQueue.tla" "$work_dir/MpmcQueue.tla"

for config in \
    "$root_dir/verification/MpmcQueue-cap1.cfg" \
    "$root_dir/verification/MpmcQueue.cfg" \
    "$root_dir/verification/MpmcQueue-cap3.cfg"
do
    name="$(basename "$config" .cfg)"
    current_config="$name"
    metadir="$work_dir/meta-$name"
    mkdir -p "$metadir"
    echo "==> $name"
    java -XX:+UseParallelGC -jar "$jar" \
        -config "$config" \
        -metadir "$metadir" \
        -dumpTrace json "$work_dir/error-trace-$name.json" \
        "$work_dir/MpmcQueue.tla"
done
