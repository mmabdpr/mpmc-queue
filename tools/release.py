#!/usr/bin/env python3
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
"""Validate, document, and package tagged mpmc_queue releases."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STABLE_TAG = re.compile(
    r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$"
)
HEADER_PATHS = (
    "include",
    "LICENSE",
    "LICENSES",
    "NOTICE",
    "README.md",
    "CHANGELOG.md",
    "SECURITY.md",
    "docs/design.md",
    "verification/README.md",
    "verification/MpmcQueue.tla",
    "verification/MpmcQueue.cfg",
    "verification/MpmcQueue-cap1.cfg",
    "verification/MpmcQueue-cap3.cfg",
    "verification/run-tlc.sh",
    "benchmarks/comparison/README.md",
    "benchmarks/comparison/RESULTS.md",
    "benchmarks/comparison/SPSC.md",
    "benchmarks/comparison/reference.json",
)


class ReleaseError(RuntimeError):
    """A release invariant was not satisfied."""


def _run(command: list[str], *, text: bool = True) -> str | bytes:
    completed = subprocess.run(
        command,
        cwd=ROOT,
        check=False,
        text=text,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        stderr = completed.stderr
        stdout = completed.stdout
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", "replace")
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", "replace")
        message = (stderr or "").strip() or (stdout or "").strip()
        if not message:
            message = "command failed"
        raise ReleaseError(message)
    return completed.stdout


def _git(*arguments: str) -> str:
    return str(_run(["git", *arguments])).strip()


def _tag_version(tag: str) -> str:
    match = STABLE_TAG.fullmatch(tag)
    if match is None:
        raise ReleaseError(
            "tag must be a stable annotated tag of the form "
            "vMAJOR.MINOR.PATCH"
        )
    return ".".join(match.groups())


def _tag_commit(tag: str) -> str:
    tag_ref = f"refs/tags/{tag}"
    try:
        tag_type = _git("cat-file", "-t", tag_ref)
    except ReleaseError as error:
        raise ReleaseError(f"tag {tag} does not exist") from error
    if tag_type != "tag":
        raise ReleaseError(f"tag {tag} must be annotated")
    return _git("rev-parse", f"{tag_ref}^{{commit}}")


def _require_on_main(commit: str) -> None:
    completed = subprocess.run(
        [
            "git",
            "merge-base",
            "--is-ancestor",
            commit,
            "refs/remotes/origin/main",
        ],
        cwd=ROOT,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 1:
        raise ReleaseError(
            f"tag commit {commit} is not reachable from origin/main"
        )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or (
            "fetch origin/main before validating the release"
        )
        raise ReleaseError(detail)


def _commit_file(commit: str, path: str) -> str:
    return _git("show", f"{commit}:{path}")


def _project_version(cmake: str) -> str:
    matches = re.findall(
        r"^project\(mpmc_queue\s+VERSION\s+(\d+\.\d+\.\d+)\b",
        cmake,
        re.MULTILINE,
    )
    if len(matches) != 1:
        raise ReleaseError(
            "CMakeLists.txt must contain exactly one project VERSION"
        )
    return matches[0]


def _header_version(header: str) -> str:
    values: list[str] = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        matches = re.findall(
            rf"^#define MPMC_VERSION_{field} ([0-9]+)$",
            header,
            re.MULTILINE,
        )
        if len(matches) != 1:
            raise ReleaseError(
                f"version.hpp must contain exactly one MPMC_VERSION_{field}"
            )
        values.append(matches[0])
    strings = re.findall(
        r'^#define MPMC_VERSION_STRING "([^"]+)"$',
        header,
        re.MULTILINE,
    )
    if len(strings) != 1:
        raise ReleaseError(
            "version.hpp must contain exactly one MPMC_VERSION_STRING"
        )
    numeric = ".".join(values)
    if strings[0] != numeric:
        raise ReleaseError(
            "MPMC_VERSION_STRING does not match the numeric version macros"
        )
    return numeric


def _changelog_section(changelog: str, version: str) -> str:
    heading = re.compile(
        rf"^## {re.escape(version)}(?: - [^\r\n]*)?[ \t]*$",
        re.MULTILINE,
    )
    matches = list(heading.finditer(changelog))
    if len(matches) != 1:
        raise ReleaseError(
            f"CHANGELOG.md must contain exactly one ## {version} section"
        )
    start = matches[0].end()
    following = re.search(r"^## ", changelog[start:], re.MULTILINE)
    end = start + following.start() if following is not None else len(changelog)
    body = changelog[start:end].strip()
    if not body:
        raise ReleaseError(f"CHANGELOG.md section ## {version} is empty")
    return body


def validate_tag(tag: str) -> tuple[str, str]:
    version = _tag_version(tag)
    commit = _tag_commit(tag)
    _require_on_main(commit)

    cmake_version = _project_version(_commit_file(commit, "CMakeLists.txt"))
    header_version = _header_version(
        _commit_file(commit, "include/mpmc/version.hpp")
    )
    if cmake_version != version:
        raise ReleaseError(
            f"CMake project version is {cmake_version}, expected {version}"
        )
    if header_version != version:
        raise ReleaseError(
            f"version.hpp version is {header_version}, expected {version}"
        )

    _changelog_section(_commit_file(commit, "CHANGELOG.md"), version)
    return version, commit


def release_notes(tag: str) -> str:
    version, commit = validate_tag(tag)
    body = _changelog_section(_commit_file(commit, "CHANGELOG.md"), version)
    return (
        f"{body}\n\n"
        "## Install\n\n"
        "Extract the source archive and install the CMake package. The test and\n"
        "example targets default to ON for a top-level build; the explicit options\n"
        "below disable them for an install-only build. `/usr/local` may require\n"
        "elevated permissions.\n\n"
        "```sh\n"
        f"tar -xzf mpmc_queue-{version}.tar.gz\n"
        f"cmake -S mpmc_queue-{version} -B build \\\n"
        "    -DCMAKE_BUILD_TYPE=Release \\\n"
        "    -DMPMC_BUILD_TESTS=OFF -DMPMC_BUILD_EXAMPLES=OFF\n"
        "cmake --install build --prefix /usr/local\n"
        "```\n\n"
        "The headers archive can be used by adding its `include/` directory to "
        "the compiler include path. The source archive provides the CMake and "
        "pkg-config metadata.\n\n"
        "## Downloads\n\n"
        f"- `mpmc_queue-{version}.tar.gz`: complete tagged source tree with "
        "CMake and pkg-config packaging.\n"
        f"- `mpmc_queue-{version}-headers.tar.gz`: public headers and "
        "documentation for direct include-path use.\n"
        "- `SHA256SUMS`: SHA-256 checksums for the two archives above. The "
        "automatically generated GitHub source archives are not included.\n\n"
        "Download both archives and `SHA256SUMS` into the same directory, then "
        "run the command for your platform:\n\n"
        "```sh\n"
        "# Linux\n"
        "sha256sum -c SHA256SUMS\n"
        "```\n\n"
        "```sh\n"
        "# macOS\n"
        "shasum -a 256 -c SHA256SUMS\n"
        "```\n\n"
        "The immutable release can also be verified through GitHub:\n\n"
        "```sh\n"
        f"gh release verify {tag} -R mmabdpr/mpmc-queue\n"
        "```\n\n"
        "Tested on Ubuntu 26.04 (GCC 15.2 and Clang 22.1) and macOS 26 "
        "(Apple Clang 21.0) with C++20 and CMake 4.5.0. The library requires "
        "CMake 3.21 or newer. Other POSIX systems are untested. Windows and "
        "MSVC are not supported. The release is licensed under CPAL-1.0; see "
        "`LICENSE` and `NOTICE`.\n"
    )


def _archive(
    commit: str,
    output: Path,
    prefix: str,
    paths: tuple[str, ...],
) -> None:
    if output.exists():
        output.unlink()
    command = [
        "git",
        "-c",
        "tar.tar.gz.command=gzip -cn",
        "-c",
        "tar.umask=0002",
        "-c",
        "core.autocrlf=false",
        "archive",
        "--format=tar.gz",
        f"--prefix={prefix}/",
        "-o",
        str(output),
        commit,
        *paths,
    ]
    _run(command)


def package_release(tag: str, destination: str) -> None:
    version, commit = validate_tag(tag)
    dist = Path(destination).resolve()
    dist.mkdir(parents=True, exist_ok=True)

    source = dist / f"mpmc_queue-{version}.tar.gz"
    headers = dist / f"mpmc_queue-{version}-headers.tar.gz"
    prefix = f"mpmc_queue-{version}"
    _archive(commit, source, prefix, ())
    _archive(commit, headers, prefix, HEADER_PATHS)

    checksum_path = dist / "SHA256SUMS"
    lines = []
    for path in (source, headers):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {path.name}\n")
    checksum_path.write_bytes("".join(lines).encode("ascii"))

    for path in (source, headers, checksum_path):
        print(path, file=sys.stderr)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate and package a tagged mpmc_queue release."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    check = subparsers.add_parser(
        "check", help="validate a tag and print its numeric version"
    )
    check.add_argument("tag")

    notes = subparsers.add_parser(
        "notes", help="write release notes for a tag"
    )
    notes.add_argument("tag")
    notes.add_argument("output")

    package = subparsers.add_parser(
        "package", help="write source, headers, and checksum archives"
    )
    package.add_argument("tag")
    package.add_argument("destination")
    return parser


def main() -> int:
    arguments = _build_parser().parse_args()
    try:
        if arguments.command == "check":
            version, commit = validate_tag(arguments.tag)
            print(
                f"tag {arguments.tag} -> {commit} ({version})",
                file=sys.stderr,
            )
            print(version)
        elif arguments.command == "notes":
            output = Path(arguments.output)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(release_notes(arguments.tag).encode("utf-8"))
        elif arguments.command == "package":
            package_release(arguments.tag, arguments.destination)
        else:
            raise ReleaseError(f"unknown command: {arguments.command}")
    except ReleaseError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
