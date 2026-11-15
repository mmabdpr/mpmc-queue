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
"""Aggregate comparison-v3 JSON lines into the canonical result artifacts."""

from __future__ import annotations

import json
import math
import os
import platform
import random
import statistics
import subprocess
import sys
from typing import NoReturn
from datetime import datetime, timezone
from pathlib import Path

SEED = 0x4D504D43
BOOTSTRAP_SAMPLES = 10_000
BASELINE = "mpmc-16B"
PRIMARY_TOPOLOGIES = [(1, 1), (2, 2), (4, 4), (4, 1), (1, 4)]
STRESS_TOPOLOGY = (8, 8)
BOOTSTRAP_SEED = SEED


def fail(message: str) -> NoReturn:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def quantile(values: list[float], probability: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = math.ceil(probability * len(ordered))
    index = min(len(ordered) - 1, max(0, rank - 1))
    return ordered[index]


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def summarize(values: list[float]) -> dict[str, object]:
    if not values:
        fail("empty sample set")
    ordered = sorted(values)
    med = median(values)
    q1 = quantile(values, 0.25)
    q3 = quantile(values, 0.75)
    deviations = sorted(abs(value - med) for value in values)
    mad = 1.4826 * median(deviations)
    rng = random.Random(BOOTSTRAP_SEED)
    boot = []
    for _ in range(BOOTSTRAP_SAMPLES):
        sample = [values[rng.randrange(len(values))]
                  for _ in range(len(values))]
        boot.append(median(sample))
    boot.sort()
    ci_low = quantile(boot, 0.025)
    ci_high = quantile(boot, 0.975)
    return {
        "median": med,
        "q1": q1,
        "q3": q3,
        "iqr": q3 - q1,
        "scaled_mad": mad,
        "min": ordered[0],
        "max": ordered[-1],
        "ci95": [ci_low, ci_high],
    }


def stable(values: list[float], elapsed: list[float]) -> tuple[bool, list[str]]:
    issues: list[str] = []
    stats = summarize(values)
    med = float(stats["median"])
    if med <= 0.0:
        issues.append("non-positive median")
    if float(stats["iqr"]) / med > 0.10:
        issues.append("IQR/median > 10%")
    if float(stats["scaled_mad"]) / med > 0.075:
        issues.append("scaled MAD/median > 7.5%")
    ci_low, ci_high = stats["ci95"]
    if (float(ci_high) - float(ci_low)) / med > 0.15:
        issues.append("bootstrap CI width > 15%")
    if len(values) >= 4:
        half = len(values) // 2
        first = median(values[:half])
        second = median(values[half:])
        if abs(second - first) / med > 0.10:
            issues.append("first-half/second-half drift > 10%")
    if any(value < 0.250 for value in elapsed):
        issues.append("timed trial shorter than 250 ms")
    return not issues, issues


def pair_ratio(values: list[float], baseline: list[float]) -> dict[str, object]:
    if len(values) != len(baseline):
        fail("paired samples have different round counts")
    ratios = [value / base for value, base in zip(values, baseline) if base > 0.0]
    stats = summarize(ratios)
    med = float(stats["median"])
    low, high = stats["ci95"]
    if med > 1.05 and float(low) > 1.05:
        verdict = "faster"
    elif med < 1.0 / 1.05 and float(high) < 1.0 / 1.05:
        verdict = "slower"
    else:
        verdict = "unresolved"
    return {
        "median": med,
        "ci95": [low, high],
        "verdict": verdict,
    }


def fmt(value: float, digits: int = 1) -> str:
    return f"{value:.{digits}f}"


def fmt_interval(value: float, low: float, high: float,
                 digits: int = 1) -> str:
    return f"{fmt(value, digits)} ({fmt(low, digits)}-{fmt(high, digits)})"


def cpu_model() -> str:
    if sys.platform == "darwin":
        try:
            return subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"],
                text=True, stderr=subprocess.DEVNULL).strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    elif sys.platform.startswith("linux"):
        try:
            with open("/proc/cpuinfo", encoding="utf-8") as source:
                for line in source:
                    if line.startswith("model name"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass
    return platform.processor() or "unknown"


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: render_results.py INPUT.jsonl OUTPUT.json OUTPUT.md",
              file=sys.stderr)
        return 2
    input_path, json_path, md_path = map(Path, sys.argv[1:])
    records = []
    for line_number, line in enumerate(
            input_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as error:
            fail(f"invalid JSON on line {line_number}: {error}")

    metadata = next((r for r in records if r.get("kind") == "metadata"), None)
    if metadata is None:
        fail("missing metadata record")
    host_record = next((r for r in records if r.get("kind") == "host"), None)
    excluded = [r for r in records if r.get("kind") == "excluded"]
    throughput = [r for r in records if r.get("kind") == "throughput"]
    latency = [r for r in records if r.get("kind") == "latency"]
    exact = [r for r in records if r.get("kind") == "exact"]
    sentinel = [r for r in records if r.get("kind") == "sentinel"]
    if not throughput or not latency:
        fail("missing throughput or latency records")
    if metadata.get("profile_id") != "comparison-v3":
        fail("unsupported profile")
    if metadata.get("quick"):
        fail("quick runs are diagnostic and cannot be published")
    rounds = int(metadata["rounds"])
    stress_rounds = 6
    libraries = [BASELINE, "rigtorp", "boost"]
    if len(exact) != len(libraries) * 6:
        fail(f"expected {len(libraries) * 6} exact validation records, "
             f"got {len(exact)}")
    for record in exact:
        if record.get("validation") != "ok":
            fail(f"exact validation failed for {record.get('library')}")

    groups: dict[tuple, list[dict]] = {}
    for record in throughput:
        if record.get("validation") != "ok":
            fail(f"validation failed for {record.get('library')}")
        key = (record["library"], record["producers"],
               record["consumers"], record["classification"])
        groups.setdefault(key, []).append(record)
    latency_groups: dict[str, list[dict]] = {}
    for record in latency:
        if record.get("validation") != "ok":
            fail(f"latency validation failed for {record.get('library')}")
        latency_groups.setdefault(record["library"], []).append(record)

    baseline_rates: dict[tuple[int, int], list[float]] = {}
    cases: list[dict] = []
    for key, records_for_case in sorted(groups.items()):
        library, producers, consumers, classification = key
        expected = stress_rounds if classification == "stress" else rounds
        if len(records_for_case) != expected:
            fail(f"expected {expected} rounds for {library} "
                 f"{producers}P/{consumers}C, got {len(records_for_case)}")
        records_for_case.sort(key=lambda row: int(row["round"]))
        rates = [float(row["rate_Mitems_s"]) for row in records_for_case]
        elapsed = [float(row["elapsed_seconds"]) for row in records_for_case]
        start_skew = max(float(row["start_skew_seconds"])
                         for row in records_for_case)
        skew_limit = 0.10 if classification == "stress" else 0.01
        if start_skew / median(elapsed) > skew_limit:
            fail(f"start skew exceeds {skew_limit:.0%} for {library} "
                 f"{producers}P/{consumers}C")
        stats = summarize(rates)
        stable_ok, issues = stable(rates, elapsed)
        if classification == "primary" and not stable_ok:
            # Symmetric cases are the ranked result.  Asymmetric cases are
            # sensitive to macOS scheduler placement; keep them in the
            # report as explicitly labeled diagnostics instead of discarding
            # an otherwise valid run.
            if producers == consumers:
                samples = ", ".join(f"{value:.3f}" for value in rates)
                fail(f"unstable symmetric primary case {library} "
                     f"{producers}P/{consumers}C: " + "; ".join(issues)
                     + f"; median={stats['median']:.3f} "
                       f"iqr={stats['iqr']:.3f} "
                       f"scaled_mad={stats['scaled_mad']:.3f} "
                       f"rates=[{samples}]")
        case = {
            "classification": classification,
            "library": library,
            "producers": producers,
            "consumers": consumers,
            "capacity": int(records_for_case[0]["capacity"]),
            "rounds": len(records_for_case),
            "total_items": int(records_for_case[0]["total_items"]),
            "median_Mitems_s": stats["median"],
            "q1_Mitems_s": stats["q1"],
            "q3_Mitems_s": stats["q3"],
            "iqr_Mitems_s": stats["iqr"],
            "scaled_mad_Mitems_s": stats["scaled_mad"],
            "min_Mitems_s": stats["min"],
            "max_Mitems_s": stats["max"],
            "ci95_Mitems_s": stats["ci95"],
            "elapsed_seconds_median": median(elapsed),
            "start_skew_seconds_max": start_skew,
            "push_failures": sum(int(row["push_failures"])
                                 for row in records_for_case),
            "pop_failures": sum(int(row["pop_failures"])
                                for row in records_for_case),
            "stability": "stable" if stable_ok else "diagnostic-unstable",
            "stability_issues": issues,
            "ratio_to_mpmc": None,
            "ratio_ci95": None,
            "verdict": "baseline" if library == BASELINE else "unresolved",
        }
        if library == BASELINE:
            baseline_rates[(producers, consumers)] = rates
        cases.append(case)

    for case in cases:
        if case["library"] == BASELINE:
            continue
        base = baseline_rates.get((case["producers"], case["consumers"]))
        if base is None:
            fail("missing mpmc baseline for paired ratio")
        values = [
            float(row["rate_Mitems_s"]) for row in groups[
                (case["library"], case["producers"], case["consumers"],
                 case["classification"])]
        ]
        ratio = pair_ratio(values, base)
        case["ratio_to_mpmc"] = ratio["median"]
        case["ratio_ci95"] = ratio["ci95"]
        case["verdict"] = (
            "diagnostic" if case["stability"] != "stable"
            else ratio["verdict"])

    latency_cases: list[dict] = []
    for library in [BASELINE, "rigtorp", "boost"]:
        rows = latency_groups.get(library)
        if rows is None:
            fail(f"missing latency group for {library}")
        if len(rows) != rounds:
            fail(f"expected {rounds} latency rounds for {library}, "
                 f"got {len(rows)}")
        rows.sort(key=lambda row: int(row["round"]))
        p50 = [float(row["p50_ns"]) for row in rows]
        p99 = [float(row["p99_ns"]) for row in rows]
        p999 = [float(row["p999_ns"]) for row in rows]
        means = [float(row["mean_ns"]) for row in rows]
        p50_stats = summarize(p50)
        p99_stats = summarize(p99)
        latency_cases.append({
            "library": library,
            "samples_per_round": int(rows[0]["samples"]),
            "rounds": len(rows),
            "median_round_mean_ns": median(means),
            "median_round_p50_ns": p50_stats["median"],
            "median_round_p90_ns": median([float(row["p90_ns"])
                                           for row in rows]),
            "median_round_p99_ns": p99_stats["median"],
            "median_round_p999_ns": median(p999),
            "p50_iqr_ns": p50_stats["iqr"],
            "p99_iqr_ns": p99_stats["iqr"],
            "p50_ci95_ns": p50_stats["ci95"],
            "p99_ci95_ns": p99_stats["ci95"],
            "maximum_observed_ns": max(float(row["max_ns"]) for row in rows),
            "quantile_method": "nearest-rank: ceil(p*n)-1",
            "stability": (
                "stable"
                if (float(p50_stats["iqr"]) / float(p50_stats["median"])
                    <= 0.10 or float(p50_stats["iqr"]) <= 83.4)
                and (float(p99_stats["iqr"]) / float(p99_stats["median"])
                     <= 0.20 or float(p99_stats["iqr"]) <= 83.4)
                else "unstable-tail"),
        })

    diagnostic_cases = [
        f"{case['library']} {case['producers']}P/{case['consumers']}C: "
        + "; ".join(case["stability_issues"])
        for case in cases
        if case["stability"] != "stable"
    ]
    status = "valid_with_diagnostics" if diagnostic_cases else "valid"

    host = platform.uname()
    canonical = {
        "schema_version": 3,
        "profile_id": "comparison-v3",
        "status": status,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "metadata": metadata,
        "host": {
            "system": host.system,
            "release": host.release,
            "machine": host.machine,
            "processor": host.processor,
            "cpu_model": cpu_model(),
            "python_cpu_count": os.cpu_count(),
        },
        "host_load": host_record,
        "excluded": excluded,
        "exact_validation": exact,
        "sentinel": sentinel,
        "throughput": cases,
        "latency": latency_cases,
        "notes": {
            "metric": "timed_checksum_transfer_Mitems_s",
            "validation": (
                "Timed trials use count plus 64-bit sum and sum-of-squares "
                "over every transferred item; a separate 1,048,576-item "
                "bitmap pass covers every library/topology exactly."
            ),
            "baseline": BASELINE,
            "stress_topology": "8P/8C",
            "latency": "closed-loop two-queue round trip",
            "eligibility": "erez-16B excluded from validated ranking",
            "diagnostics": diagnostic_cases,
        },
    }

    def lookup(topology: tuple[int, int], library: str,
               classification: str = "primary") -> dict:
        for case in cases:
            if (case["classification"] == classification
                    and case["library"] == library
                    and (case["producers"], case["consumers"]) == topology):
                return case
        fail(f"missing case {library} {topology}")

    lines: list[str] = []
    lines.append("# Cross-library benchmark results")
    lines.append("")
    if diagnostic_cases:
        lines.append("Status: **reference run with diagnostics**. This")
        lines.append("file is generated from `reference.json`; both artifacts")
        lines.append("share the same profile and run metadata. Asymmetric")
        lines.append("cases that do not meet the primary stability bounds are")
        lines.append("reported as diagnostics rather than used for a ranking")
        lines.append("claim.")
    else:
        lines.append("Status: **reference run**. This file is generated")
        lines.append("from `reference.json`; both artifacts share the same")
        lines.append("profile and run metadata.")
    lines.append("")
    lines.append("## Environment")
    lines.append("")
    lines.append("| Item | Value |")
    lines.append("| --- | --- |")
    lines.append(f"| Profile | `{metadata['profile_id']}` |")
    lines.append(f"| mpmc_queue | `{metadata['mpmc_version']}` |")
    lines.append(f"| Boost.Lockfree | `{metadata['boost_version']}` |")
    lines.append(f"| Compiler | `{metadata['compiler']}` |")
    lines.append(f"| CPU | {cpu_model()} |")
    lines.append(f"| OS | `{host.system} {host.release} ({host.machine})` |")
    lines.append(f"| Hardware threads | {metadata['hardware_threads']} |")
    lines.append(f"| Start load average | "
                 f"{float(metadata['load1_start']):.2f} |")
    lines.append(f"| Host load limit | "
                 f"{float(metadata['max_load']):.2f} |")
    if host_record is not None:
        lines.append(f"| Primary load peak | "
                     f"{float(host_record['load1_peak_primary']):.2f} |")
        lines.append(f"| Stress load peak | "
                     f"{float(host_record['load1_peak_stress']):.2f} |")
    lines.append(f"| Clock tick | ~{fmt(float(metadata['clock_tick_ns']))} ns |")
    lines.append(f"| Placement | {metadata['placement']} |")
    lines.append(f"| Throughput capacity | {metadata['throughput_capacity']} |")
    lines.append(f"| Latency capacity | {metadata['latency_capacity']} |")
    lines.append(f"| Primary rounds | {metadata['rounds']} |")
    lines.append(f"| Latency samples per round | {metadata['latency_samples']} |")
    lines.append("")
    lines.append("The load peak is sampled during the run and includes the")
    lines.append("benchmark's own workers; the start load is the host gate.")
    if metadata.get("busy_host_override"):
        lines.append("The start load was above the default limit; this run")
        lines.append("used `--allow-busy-host`. The symmetric primary cases")
        lines.append("still met the stability bounds.")
    lines.append("")
    lines.append("Boost.Lockfree and rigtorp use different internal metadata")
    lines.append("layouts. mpmc-16B uses a 16-byte atomic cell with an 8-byte")
    lines.append("payload and a 32-bit index. The comparison is of queue APIs")
    lines.append("and behavior, not byte-for-byte identical storage.")
    lines.append("")
    workload_rows = metadata.get("workload_items")
    if workload_rows:
        lines.append("Calibrated items per trial (millions), one value per")
        lines.append("topology in the order 1P/1C, 2P/2C, 4P/4C, 4P/1C, 1P/4C,")
        lines.append("8P/8C:")
        lines.append("")
        lines.append("| Queue | 1P/1C | 2P/2C | 4P/4C | 4P/1C | 1P/4C | 8P/8C |")
        lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
        queue_names = [BASELINE, "rigtorp", "boost"]
        for index, library in enumerate(queue_names):
            values = workload_rows[index]
            cells = [f"{float(value) / 1.0e6:.2f}" for value in values]
            lines.append(f"| {library} | " + " | ".join(cells) + " |")
        lines.append("")
    lines.append("## Primary throughput")
    lines.append("")
    lines.append("Timed transfer throughput in millions of items per second,")
    lines.append("median with interquartile range in parentheses. One item is")
    lines.append("one successful enqueue and one successful dequeue. Timed")
    lines.append("workers validate every value with count, wrapped 64-bit sum,")
    lines.append("and wrapped sum-of-squares. A separate untimed pass checks")
    lines.append("every library and topology item-by-item with an exact bitmap.")
    lines.append("")
    lines.append("| Queue | 1P/1C | 2P/2C | 4P/4C | 4P/1C | 1P/4C |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: |")
    for library in [BASELINE, "rigtorp", "boost"]:
        cells = []
        for topology in PRIMARY_TOPOLOGIES:
            case = lookup(topology, library)
            cell = fmt_interval(case["median_Mitems_s"],
                                case["q1_Mitems_s"],
                                case["q3_Mitems_s"])
            if case["stability"] != "stable":
                cell += "*"
            cells.append(cell)
        lines.append(f"| {library} | " + " | ".join(cells) + " |")
    if diagnostic_cases:
        lines.append("")
        lines.append("* Diagnostic only: this asymmetric case did not meet")
        lines.append("  the primary stability bounds on this host. The value")
        lines.append("  is reported but is not used as a ranking claim.")
    lines.append("")
    lines.append("Paired ratios use the same round for each comparison. A")
    lines.append("difference is called clear only when the bootstrap 95%")
    lines.append("interval lies wholly outside the 0.95-1.05 equivalence band.")
    lines.append("The ratio is the median of paired per-round ratios, not the")
    lines.append("ratio of the displayed medians; rounding can make the two")
    lines.append("differ slightly.")
    lines.append("")
    lines.append("| Queue | 1P/1C | 2P/2C | 4P/4C | 4P/1C | 1P/4C |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: |")
    for library in [BASELINE, "rigtorp", "boost"]:
        cells = []
        for topology in PRIMARY_TOPOLOGIES:
            case = lookup(topology, library)
            if library == BASELINE:
                cells.append("baseline")
            else:
                cell = (f"{case['ratio_to_mpmc']:.2f}x "
                        f"({case['ratio_ci95'][0]:.2f}-"
                        f"{case['ratio_ci95'][1]:.2f}), "
                        f"{case['verdict']}")
                if case["stability"] != "stable":
                    cell += "*"
                cells.append(cell)
        lines.append(f"| {library} | " + " | ".join(cells) + " |")
    lines.append("")
    lines.append("## 8P/8C scheduler-stress")
    lines.append("")
    lines.append("The 8P/8C case runs sixteen workers on a 14-thread host and")
    lines.append("is reported separately. It measures the queue plus scheduler")
    lines.append("and cache contention; it is not a scaling claim.")
    lines.append("")
    lines.append("| Queue | Median Mitems/s | IQR | Ratio to mpmc |")
    lines.append("| --- | ---: | ---: | ---: |")
    for library in [BASELINE, "rigtorp", "boost"]:
        case = lookup(STRESS_TOPOLOGY, library, "stress")
        ratio = "baseline" if library == BASELINE else (
            f"{case['ratio_to_mpmc']:.2f}x")
        lines.append(f"| {library} | {fmt(case['median_Mitems_s'])} | "
                     f"{fmt(case['iqr_Mitems_s'])} | {ratio} |")
    lines.append("")
    lines.append("## Ping-pong latency")
    lines.append("")
    lines.append("Closed-loop round-trip nanoseconds between two 64-entry")
    lines.append("queues. Workers busy-spin. Values are medians of per-round")
    lines.append("quantiles; brackets show the interquartile range across")
    lines.append("rounds. Maximum is diagnostic, not an upper bound.")
    lines.append("")
    lines.append("| Queue | Mean | p50 | p99 | p99.9 | Max |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: |")
    for case in latency_cases:
        lines.append(
            f"| {case['library']} | {fmt(case['median_round_mean_ns'])} | "
            f"{fmt(case['median_round_p50_ns'])} "
            f"[{fmt(case['p50_iqr_ns'])}] | "
            f"{fmt(case['median_round_p99_ns'])} "
            f"[{fmt(case['p99_iqr_ns'])}] | "
            f"{fmt(case['median_round_p999_ns'])} | "
            f"{fmt(case['maximum_observed_ns'])} |")
    lines.append("")
    lines.append(f"The measured steady-clock tick is about "
                 f"{fmt(float(metadata['clock_tick_ns']))} ns. Differences")
    lines.append("smaller than two ticks are below the timer's practical")
    lines.append("resolution.")
    lines.append("")
    lines.append("## What the differences mean")
    lines.append("")
    lines.append("These are observations from this workload, not universal")
    lines.append("rankings. The measured medians for the symmetric primary")
    lines.append("topologies are:")
    lines.append("")
    for producers, consumers in [(1, 1), (2, 2), (4, 4)]:
        cells = []
        for library in [BASELINE, "rigtorp", "boost"]:
            case = lookup((producers, consumers), library)
            if library == BASELINE:
                cells.append(
                    f"mpmc-16B {fmt(case['median_Mitems_s'])} M items/s")
            else:
                cells.append(
                    f"{library} {fmt(case['median_Mitems_s'])} M items/s "
                    f"({case['ratio_to_mpmc']:.2f}x, {case['verdict']})")
        lines.append(f"- {producers}P/{consumers}C: " + "; ".join(cells) + ".")
    lines.append("")
    lines.append("The relevant implementation facts are:")
    lines.append("")
    lines.append("- mpmc-16B publishes payload and sequence with one 16-byte")
    lines.append("  compare-exchange, then advances the shared index with a")
    lines.append("  monotonic relaxed compare-exchange. rigtorp uses a split")
    lines.append("  head/tail index with an 8-byte compare-exchange and a")
    lines.append("  separate slot publication store; its pinned revision uses")
    lines.append("  64-byte slot spacing on this 128-byte-cache-line host. Those")
    lines.append("  differences change contention and footprint, but this")
    lines.append("  benchmark does not isolate the contribution of CAS width,")
    lines.append("  stride, index update, or memory order.")
    lines.append("- At higher contention the differences narrow and become")
    lines.append("  sensitive to cache-line ownership and scheduling. The 8P/8C")
    lines.append("  row is explicitly a scheduler-stress measurement.")
    lines.append("- Boost.Lockfree uses linked nodes, tagged handles, and a")
    lines.append("  reusable freelist; it performs more metadata operations per")
    lines.append("  transfer than a direct slot ring.")
    lines.append("")
    lines.append("## Exact validation")
    lines.append("")
    lines.append("Before the timed rounds, every library and topology transferred")
    lines.append("1,048,576 unique values through an exact per-consumer bitmap.")
    lines.append(f"All {len(exact)} exact cases reported `ok`. This pass is")
    lines.append("outside the timed region and is not part of the rate above.")
    lines.append("")
    lines.append("## Excluded library")
    lines.append("")
    for record in excluded:
        lines.append(f"- `{record['library']}`: {record['reason']}.")
    lines.append("")
    lines.append("The upstream erez-strauss source is not part of the validated")
    lines.append("ranking. A corrected fork would need a separate name, patch")
    lines.append("hash, and fresh reference run.")
    lines.append("")
    lines.append("## Reproduce")
    lines.append("")
    lines.append("```sh")
    lines.append("./benchmarks/comparison/run.sh")
    lines.append("```")
    lines.append("")
    lines.append("The runner configures a Release build, fetches the pinned")
    lines.append("dependency, runs both the general and specialized 1P/1C")
    lines.append("profiles, then publishes `reference.json`, this file, and")
    lines.append("`SPSC.md` with same-filesystem renames. A failed or unstable run")
    lines.append("leaves the previous reference untouched. `--quick` is diagnostic")
    lines.append("only and never publishes.")
    lines.append("")

    json_path.write_text(json.dumps(canonical, indent=2) + "\n",
                         encoding="utf-8")
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
