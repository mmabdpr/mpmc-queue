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

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=toolchain.lock
source "$root/ci/toolchain.lock"

fail() {
    echo "toolchain check failed: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "missing command '$1'"
}

check_gcc() {
    require_command gcc-15
    require_command g++-15
    local cc cxx
    cc="$(gcc-15 -dumpfullversion)"
    cxx="$(g++-15 -dumpfullversion)"
    [[ "$cc" == "$GCC_VERSION" ]] ||
        fail "gcc-15 is $cc, expected $GCC_VERSION"
    [[ "$cxx" == "$GCC_VERSION" ]] ||
        fail "g++-15 is $cxx, expected $GCC_VERSION"
    printf 'gcc: %s, g++: %s\n' "$cc" "$cxx"
}

check_clang() {
    require_command clang-22
    require_command clang++-22
    local cc cxx
    cc="$(clang-22 --version | sed -n '1p')"
    cxx="$(clang++-22 --version | sed -n '1p')"
    [[ "$cc" == *"clang version $CLANG_VERSION"* ]] ||
        fail "clang-22 is '$cc', expected $CLANG_VERSION"
    [[ "$cxx" == *"clang version $CLANG_VERSION"* ]] ||
        fail "clang++-22 is '$cxx', expected $CLANG_VERSION"
    printf 'clang: %s\nclang++: %s\n' "$cc" "$cxx"
}

check_appleclang() {
    require_command xcrun
    local cc cxx
    cc="$(xcrun clang --version | sed -n '1p')"
    cxx="$(xcrun clang++ --version | sed -n '1p')"
    [[ "$cc" == *"Apple clang version $APPLECLANG_VERSION"* ]] ||
        fail "Apple clang is '$cc', expected $APPLECLANG_VERSION"
    [[ "$cxx" == *"Apple clang version $APPLECLANG_VERSION"* ]] ||
        fail "Apple clang++ is '$cxx', expected $APPLECLANG_VERSION"
    printf 'apple clang: %s\napple clang++: %s\n' "$cc" "$cxx"
}

check_cmake() {
    require_command cmake
    local version
    version="$(cmake --version | sed -n '1s/^cmake version //p')"
    [[ "$version" == "$CMAKE_VERSION" ]] ||
        fail "cmake is $version, expected $CMAKE_VERSION"
    printf 'cmake: %s\n' "$version"
}

check_python() {
    require_command python3
    local version
    version="$(python3 -c 'import sys; print(".".join(map(str, sys.version_info[:3])))')"
    [[ "$version" == "$PYTHON_VERSION" ]] ||
        fail "python3 is $version, expected $PYTHON_VERSION"
    printf 'python3: %s\n' "$version"
}

check_java() {
    require_command java
    java -version >/dev/null 2>&1 || fail "java did not run"
    local output version
    output="$(java -version 2>&1)"
    version="${JDK_VERSION%%+*}"
    grep -Fq "\"$version\"" <<<"$output" ||
        fail "java is not $JDK_VERSION: $output"
    grep -Fq "$JDK_VERSION" <<<"$output" ||
        fail "java build is not $JDK_VERSION: $output"
    printf 'java: %s\n' "$version"
}

check_clang_format() {
    require_command clang-format
    local version
    version="$(clang-format --version | sed -n '1s/^clang-format version //p')"
    [[ "$version" == "$CLANG_FORMAT_VERSION" ]] ||
        fail "clang-format is $version, expected $CLANG_FORMAT_VERSION"
    printf 'clang-format: %s\n' "$version"
}

check_jq() {
    require_command jq
    local version
    version="$(jq --version)"
    [[ "$version" == "jq-$JQ_VERSION" ]] ||
        fail "jq is $version, expected jq-$JQ_VERSION"
    printf 'jq: %s\n' "$version"
}

check_gh() {
    require_command gh
    local version
    version="$(gh --version | sed -n '1s/^gh version \([^ ]*\).*$/\1/p')"
    [[ "$version" == "$GH_VERSION" ]] ||
        fail "gh is $version, expected $GH_VERSION"
    printf 'gh: %s\n' "$version"
}

check_pkg_config() {
    require_command pkg-config
    local version
    version="$(pkg-config --version)"
    [[ "$version" == "$PKG_CONFIG_VERSION" ]] ||
        fail "pkg-config is $version, expected $PKG_CONFIG_VERSION"
    printf 'pkg-config: %s\n' "$version"
}

if [[ $# -eq 0 ]]; then
    echo "usage: $0 <tool>..." >&2
    exit 2
fi

printf 'runner: %s %s\n' "$(uname -s)" "$(uname -m)"
printf 'image: %s %s\n' \
    "${ImageOS:-unknown}" "${ImageVersion:-unknown}"

for tool in "$@"; do
    case "$tool" in
        gcc) check_gcc ;;
        clang) check_clang ;;
        appleclang) check_appleclang ;;
        cmake) check_cmake ;;
        python) check_python ;;
        java) check_java ;;
        clang-format) check_clang_format ;;
        jq) check_jq ;;
        gh) check_gh ;;
        pkg-config) check_pkg_config ;;
        *)
            fail "unknown tool '$tool'"
            ;;
    esac
done
