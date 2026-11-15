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

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps="$here/deps"
mkdir -p "$deps"

fetch()
{
    url=$1
    dir=$2
    revision=$3
    if [ ! -d "$dir/.git" ]; then
        git clone "$url" "$dir"
    fi
    # A pinned SHA is meaningful only for a clean checkout.  Reject a
    # dependency tree with local modifications.
    if [ -n "$(git -C "$dir" status --porcelain --untracked-files=all)" ]; then
        echo "error: $dir contains local changes; remove it or clean it" >&2
        exit 1
    fi
    actual=$(git -C "$dir" rev-parse HEAD)
    if [ "$actual" != "$revision" ]; then
        # The pinned object may already be present even when HEAD points at a
        # different commit.  Fetch only when it is not present, so a previously
        # populated checkout remains usable offline.
        if ! git -C "$dir" rev-parse --verify --quiet \
            "$revision^{commit}" >/dev/null; then
            git -C "$dir" fetch --quiet origin
        fi
        git -C "$dir" checkout --quiet "$revision"
        actual=$(git -C "$dir" rev-parse HEAD)
    fi
    if [ "$actual" != "$revision" ]; then
        echo "error: $dir is at $actual, expected $revision" >&2
        exit 1
    fi
    if [ -n "$(git -C "$dir" status --porcelain --untracked-files=all)" ]; then
        echo "error: $dir is not clean after checking out $revision" >&2
        exit 1
    fi
    printf '%s %s\n' "$(basename "$dir")" "$actual"
}

fetch https://github.com/rigtorp/MPMCQueue.git \
    "$deps/MPMCQueue" b9808ede08f26fa9df4df4e081d19cace8f6c6ea
