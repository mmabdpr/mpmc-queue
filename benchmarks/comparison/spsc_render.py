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
"""Summarize the specialized 1P/1C SPSC throughput profile."""

from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print("usage: spsc_render.py INPUT.jsonl [OUTPUT.md]", file=sys.stderr)
        return 2
    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2]) if len(sys.argv) == 3 else None
    metadata = None
    rows: dict[str, list[dict]] = {}
    for line in input_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        record = json.loads(line)
        if record.get("kind") == "spsc-metadata":
            metadata = record
        elif record.get("kind") == "spsc":
            if record.get("validation") != "ok":
                print(f"error: FIFO validation failed for {record['library']}",
                      file=sys.stderr)
                return 1
            rows.setdefault(record["library"], []).append(record)
    if metadata is None or not rows:
        print("error: missing metadata or trial records", file=sys.stderr)
        return 1
    rounds = int(metadata["rounds"])
    for library, trials in rows.items():
        if len(trials) != rounds:
            print(f"error: {library} has {len(trials)} of {rounds} trials",
                  file=sys.stderr)
            return 1
        if any(float(trial["seconds"]) < 0.250 for trial in trials):
            print(f"error: {library} has a trial shorter than 250 ms",
                  file=sys.stderr)
            return 1

    lines = [
        "# SPSC throughput profile",
        "",
        "This profile is a topology-specific diagnostic, not part of the",
        "stability-gated general ranking. It runs one producer and one",
        "consumer with strict FIFO validation.",
        "",
        f"- Host threads: {metadata['hardware_threads']}",
        f"- Capacity: {metadata['capacity']}",
        f"- Items per trial: {metadata['items']}",
        f"- Rounds: {metadata['rounds']}",
        f"- Compiler: {metadata['compiler']}",
        "",
        "| Queue | Median Mitems/s | IQR | Min | Max |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for library, trials in rows.items():
        values = sorted(float(trial["Mitems_s"]) for trial in trials)
        median = statistics.median(values)
        q1 = values[(len(values) - 1) // 4]
        q3 = values[(3 * (len(values) - 1)) // 4]
        lines.append(
            f"| {library} | {median:.1f} | {q3 - q1:.1f} | "
            f"{values[0]:.1f} | {values[-1]:.1f} |"
        )
    lines.extend([
        "",
        "The profile uses one producer and one consumer with strict sequential",
        "FIFO validation and no per-item bitmap, starting from an empty queue",
        "after warm-up. `mpmc-spsc` publishes a separate payload/sequence pair",
        "with a release store. The profile is a topology-specific diagnostic;",
        "use the general MPMC comparison for mixed roles.",
        "",
    ])
    text = "\n".join(lines)
    if output_path is None:
        print(text)
    else:
        output_path.write_text(text + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
