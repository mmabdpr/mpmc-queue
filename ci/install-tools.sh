#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
# SPDX-License-Identifier: CPAL-1.0
#
# The contents of this file are subject to the Common Public Attribution License Version 1.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at https://opensource.org/license/cpal-1-0. The License is based on the Mozilla Public License Version 1.1 but Sections 14 and 15 have been added to cover use of software over a computer network and provide for limited attribution for the Original Developer. In addition, Exhibit A has been modified to be consistent with Exhibit B.
# Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for the specific language governing rights and limitations under the License.
# The Original Code is mpmc_queue, a header-only C++20 bounded queue library.
# The Original Developer is not separately identified and is the Initial Developer. If left blank, the Original Developer is the Initial Developer.
# The Initial Developer of the Original Code is Mohammad Mahdi Abdollahpour. All portions of the code written by Mohammad Mahdi Abdollahpour are Copyright (c) 2026. All Rights Reserved.
# Contributor: Mohammad Mahdi Abdollahpour.
#
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=toolchain.lock
source "$root/ci/toolchain.lock"

tool="${1:-}"
if [[ -z "$tool" ]]; then
    echo "usage: $0 cmake|jdk|clang-format|codeql" >&2
    exit 2
fi

tool_dir="${MPMC_TOOL_DIR:-${RUNNER_TEMP:-/tmp}/mpmc-tools}"
mkdir -p "$tool_dir"
tmp_root="$(mktemp -d)"
trap 'rm -rf "$tmp_root"' EXIT

add_path() {
    local path="$1"
    if [[ -n "${GITHUB_PATH:-}" ]]; then
        printf '%s\n' "$path" >>"$GITHUB_PATH"
    else
        printf 'Add to PATH: %s\n' "$path"
    fi
}

sha256_file() {
    local path="$1"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$path" | awk '{print $1}'
    else
        shasum -a 256 "$path" | awk '{print $1}'
    fi
}

download() {
    local url="$1"
    local expected="$2"
    local output="$3"
    curl --fail --location --silent --show-error \
        --retry 3 --retry-delay 1 "$url" --output "$output"
    local actual
    actual="$(sha256_file "$output")"
    if [[ "$actual" != "$expected" ]]; then
        echo "checksum mismatch for $url" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        exit 1
    fi
}

install_cmake() {
    local url sha dest archive bin
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64)
            url="$CMAKE_LINUX_X86_64_URL"
            sha="$CMAKE_LINUX_X86_64_SHA256"
            ;;
        Darwin-x86_64 | Darwin-arm64)
            url="$CMAKE_MACOS_UNIVERSAL_URL"
            sha="$CMAKE_MACOS_UNIVERSAL_SHA256"
            ;;
        *)
            echo "unsupported CMake platform: $(uname -s)-$(uname -m)" >&2
            exit 1
            ;;
    esac
    dest="$tool_dir/cmake-$CMAKE_VERSION"
    if [[ ! -x "$dest/bin/cmake" && ! -x "$dest/CMake.app/Contents/bin/cmake" ]]; then
        archive="$tmp_root/cmake.tar.gz"
        download "$url" "$sha" "$archive"
        rm -rf "$dest"
        mkdir -p "$dest"
        tar -xzf "$archive" -C "$dest" --strip-components=1
    fi
    if [[ -x "$dest/CMake.app/Contents/bin/cmake" ]]; then
        bin="$dest/CMake.app/Contents/bin"
    else
        bin="$dest/bin"
    fi
    add_path "$bin"
    "$bin/cmake" --version | sed -n '1p'
}

install_jdk() {
    local dest archive
    if [[ "$(uname -s)-$(uname -m)" != "Linux-x86_64" ]]; then
        echo "the pinned JDK archive is only defined for Linux x86_64" >&2
        exit 1
    fi
    dest="$tool_dir/jdk-$JDK_VERSION"
    if [[ ! -x "$dest/bin/java" ]]; then
        archive="$tmp_root/jdk.tar.gz"
        download "$JDK_LINUX_X86_64_URL" "$JDK_LINUX_X86_64_SHA256" "$archive"
        rm -rf "$dest"
        mkdir -p "$dest"
        tar -xzf "$archive" -C "$dest" --strip-components=1
    fi
    add_path "$dest/bin"
    if [[ -n "${GITHUB_ENV:-}" ]]; then
        printf 'JAVA_HOME=%s\n' "$dest" >>"$GITHUB_ENV"
    fi
    "$dest/bin/java" -version 2>&1 | sed -n '1p'
}

install_clang_format() {
    local venv
    venv="$tool_dir/clang-format-$CLANG_FORMAT_VERSION"
    if [[ ! -x "$venv/bin/clang-format" ]]; then
        python3 -m venv "$venv"
        "$venv/bin/python" -m pip install \
            --disable-pip-version-check --no-input --no-cache-dir \
            --only-binary=:all: --require-hashes --no-deps \
            -r "$root/ci/requirements-format.txt"
    fi
    ln -sf clang-format-diff.py "$venv/bin/clang-format-diff"
    add_path "$venv/bin"
    "$venv/bin/clang-format" --version
}

install_codeql() {
    local bundle
    if [[ "$(uname -s)-$(uname -m)" != "Linux-x86_64" ]]; then
        echo "the pinned CodeQL bundle is only defined for Linux x86_64" >&2
        exit 1
    fi
    bundle="$tool_dir/codeql-bundle-$CODEQL_VERSION.tar.gz"
    if [[ -f "$bundle" ]]; then
        if [[ "$(sha256_file "$bundle")" != "$CODEQL_BUNDLE_LINUX_X86_64_SHA256" ]]; then
            rm -f "$bundle"
        fi
    fi
    if [[ ! -f "$bundle" ]]; then
        download "$CODEQL_BUNDLE_LINUX_X86_64_URL" \
            "$CODEQL_BUNDLE_LINUX_X86_64_SHA256" "$bundle"
    fi
    printf '%s\n' "$bundle"
}

case "$tool" in
    cmake) install_cmake ;;
    jdk) install_jdk ;;
    clang-format) install_clang_format ;;
    codeql) install_codeql ;;
    *)
        echo "unknown tool: $tool" >&2
        exit 2
        ;;
esac
