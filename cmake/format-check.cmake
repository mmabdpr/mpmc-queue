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
# Lightweight formatting check for changed C++ files.
#
# This checks only touched files (or an explicit file list), so
# introducing the check does not force a repository-wide reformat.  It is run
# by the optional `format-check` target and by CI; it is not part of the normal
# build.
#
# Inputs:
#   MPMC_CLANG_FORMAT   path to clang-format (required)
#   MPMC_SOURCE_DIR     repository root (required)
#   MPMC_FORMAT_FILES   optional ;-list of files to check
#   MPMC_FORMAT_BASE    optional git revision to diff against (default HEAD^1)

if(NOT MPMC_CLANG_FORMAT)
    message(FATAL_ERROR "format-check: MPMC_CLANG_FORMAT is not set")
endif()
if(NOT MPMC_SOURCE_DIR)
    message(FATAL_ERROR "format-check: MPMC_SOURCE_DIR is not set")
endif()

set(_files "${MPMC_FORMAT_FILES}")

if(NOT _files)
    set(_base "${MPMC_FORMAT_BASE}")
    if(NOT _base)
        set(_base "HEAD^1")
    endif()

    execute_process(
        COMMAND git diff --name-only --diff-filter=ACMR "${_base}"
        WORKING_DIRECTORY "${MPMC_SOURCE_DIR}"
        OUTPUT_VARIABLE _diff_files
        RESULT_VARIABLE _git_result
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _git_result EQUAL 0)
        message(STATUS "format-check: no usable git revision; nothing to check")
        return()
    endif()

    # Also check unstaged edits so a local run is useful before committing.
    execute_process(
        COMMAND git diff --name-only --diff-filter=ACMR
        WORKING_DIRECTORY "${MPMC_SOURCE_DIR}"
        OUTPUT_VARIABLE _worktree_files
        RESULT_VARIABLE _worktree_result
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_worktree_result EQUAL 0)
        set(_diff_files "${_diff_files};${_worktree_files}")
    endif()

    string(REPLACE "\n" ";" _diff_list "${_diff_files}")
    foreach(_file IN LISTS _diff_list)
        if(_file MATCHES "\\.(hpp|h|hh|cpp|cc|cxx)$")
            list(APPEND _files "${_file}")
        endif()
    endforeach()
endif()

if(NOT _files)
    message(STATUS "format-check: no changed C++ files")
    return()
endif()

list(REMOVE_DUPLICATES _files)
set(_failed_files "")
foreach(_file IN LISTS _files)
    if(NOT IS_ABSOLUTE "${_file}")
        set(_absolute "${MPMC_SOURCE_DIR}/${_file}")
    else()
        set(_absolute "${_file}")
    endif()
    if(NOT EXISTS "${_absolute}")
        continue()
    endif()

    execute_process(
        COMMAND "${MPMC_CLANG_FORMAT}" --dry-run --Werror "${_absolute}"
        RESULT_VARIABLE _format_result
        OUTPUT_VARIABLE _format_output
        ERROR_VARIABLE _format_output)
    if(NOT _format_result EQUAL 0)
        message(STATUS "${_format_output}")
        list(APPEND _failed_files "${_file}")
    endif()
endforeach()

if(_failed_files)
    string(REPLACE ";" "\n  " _pretty "${_failed_files}")
    message(FATAL_ERROR
        "clang-format would change these files:\n  ${_pretty}")
endif()
message(STATUS "format-check: all checked files are formatted")
