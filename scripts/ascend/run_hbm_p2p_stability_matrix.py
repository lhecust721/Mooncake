#!/usr/bin/env python3
# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0

"""Run the HBM export/NVMe DMA mapping probe repeatedly on an A3 host."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
import time
from typing import Dict, Iterable, List


KEY_VALUE_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")


def parse_key_values(line: str) -> Dict[str, str]:
    return dict(KEY_VALUE_RE.findall(line.strip()))


def parse_sample(lines: Iterable[str]) -> Dict[str, object]:
    sample: Dict[str, object] = {}
    for line in lines:
        if line.startswith("[PASS] hbm_page_export:") or line.startswith(
            "[FAIL] hbm_page_export:"
        ):
            sample["hbm_page_export"] = line.startswith("[PASS]")
            sample.update(
                {
                    key: int(value, 0)
                    for key, value in parse_key_values(line).items()
                    if key
                    in {
                        "page_size",
                        "page_count",
                        "pa_runs",
                        "first_pa",
                        "last_pa",
                        "invalidated",
                    }
                }
            )
        elif line.startswith("[PASS] nvme_dma_api_map:") or line.startswith(
            "[FAIL] nvme_dma_api_map:"
        ):
            sample["nvme_dma_api_map"] = line.startswith("[PASS]")
            values = parse_key_values(line)
            for key in ("dma_runs", "first_iova", "last_iova", "aligned"):
                if key in values:
                    sample[key] = int(values[key], 0)
            mapped = values.get("mapped")
            if mapped and "/" in mapped:
                mapped_count, expected_count = mapped.split("/", 1)
                sample["mapped_count"] = int(mapped_count, 0)
                sample["expected_mapped_count"] = int(expected_count, 0)
        elif line.startswith("RESULT "):
            sample["result"] = parse_key_values(line)
        elif line.startswith("RUNNER_RESULT "):
            sample["runner_result"] = parse_key_values(line)
    result = sample.get("result", {})
    sample["passed"] = bool(
        isinstance(result, dict)
        and result.get("hbm_export") == "PASS"
        and result.get("nvme_device") == "PASS"
        and result.get("nvme_dma_api_map") == "PASS"
        and result.get("pages_released") == "PASS"
        and result.get("level") == "L1_5_NVME_DMA_API_MAPPED"
    )
    return sample


def module_loaded(name: str = "mooncake_hbm_p2p_probe") -> bool:
    try:
        with open("/proc/modules", encoding="utf-8") as modules:
            return any(line.startswith(name + " ") for line in modules)
    except OSError:
        return False


def stream_command(command: List[str]) -> tuple[int, List[str]]:
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    lines: List[str] = []
    assert process.stdout is not None
    for line in process.stdout:
        line = line.rstrip("\n")
        lines.append(line)
        print(line, flush=True)
    return process.wait(), lines


def parse_sizes(value: str) -> List[int]:
    sizes = [int(item.strip(), 0) for item in value.split(",") if item.strip()]
    if not sizes or any(size <= 0 or size > 256 * 1024 * 1024 for size in sizes):
        raise argparse.ArgumentTypeError(
            "sizes must be positive and no larger than 256 MiB"
        )
    if any(size % 4096 for size in sizes):
        raise argparse.ArgumentTypeError("every size must be 4-KiB aligned")
    return sizes


def summarize(samples: List[Dict[str, object]], sizes: List[int]) -> Dict[str, object]:
    by_size: Dict[str, object] = {}
    for size in sizes:
        selected = [sample for sample in samples if sample["size"] == size]
        passed = [sample for sample in selected if sample["passed"]]
        by_size[str(size)] = {
            "attempts": len(selected),
            "passed": len(passed),
            "page_count_min": min(
                (int(sample["page_count"]) for sample in passed), default=None
            ),
            "page_count_max": max(
                (int(sample["page_count"]) for sample in passed), default=None
            ),
            "pa_runs_min": min(
                (int(sample["pa_runs"]) for sample in passed), default=None
            ),
            "pa_runs_max": max(
                (int(sample["pa_runs"]) for sample in passed), default=None
            ),
            "dma_runs_min": min(
                (int(sample["dma_runs"]) for sample in passed), default=None
            ),
            "dma_runs_max": max(
                (int(sample["dma_runs"]) for sample in passed), default=None
            ),
        }
    return {
        "type": "matrix_summary",
        "attempts": len(samples),
        "passed": sum(bool(sample["passed"]) for sample in samples),
        "all_passed": all(bool(sample["passed"]) for sample in samples),
        "by_size": by_size,
    }


def write_jsonl(path: pathlib.Path, records: Iterable[Dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as output:
        for record in records:
            output.write(json.dumps(record, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--module", required=True)
    parser.add_argument("--nvme-bdf", required=True)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument(
        "--sizes",
        type=parse_sizes,
        default=parse_sizes("1048576,4194304,16777216,67108864"),
        help="comma-separated byte sizes; default: 1,4,16,64 MiB",
    )
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--runner", default="scripts/ascend/run_hbm_p2p_mapping_probe.sh")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--keep-module", action="store_true")
    args = parser.parse_args()
    if args.iterations <= 0:
        parser.error("--iterations must be positive")

    initially_loaded = module_loaded()
    samples: List[Dict[str, object]] = []
    total = len(args.sizes) * args.iterations
    sequence = 0
    try:
        for size in args.sizes:
            for iteration in range(1, args.iterations + 1):
                sequence += 1
                print(
                    f"MATRIX_BEGIN sample={sequence}/{total} size={size} "
                    f"iteration={iteration}/{args.iterations}",
                    flush=True,
                )
                command = [
                    "bash",
                    args.runner,
                    "--binary",
                    args.binary,
                    "--module",
                    args.module,
                    "--device",
                    str(args.device),
                    "--size",
                    str(size),
                    "--nvme-bdf",
                    args.nvme_bdf,
                    "--keep-module",
                ]
                exit_code, lines = stream_command(command)
                sample = parse_sample(lines)
                sample.update(
                    {
                        "type": "matrix_sample",
                        "sequence": sequence,
                        "size": size,
                        "iteration": iteration,
                        "exit_code": exit_code,
                        "timestamp_ns": time.time_ns(),
                    }
                )
                samples.append(sample)
                print(
                    "MATRIX_SAMPLE "
                    f"size={size} iteration={iteration} "
                    f"status={'PASS' if sample['passed'] else 'FAIL'} "
                    f"page_count={sample.get('page_count', 'NA')} "
                    f"pa_runs={sample.get('pa_runs', 'NA')} "
                    f"dma_runs={sample.get('dma_runs', 'NA')}",
                    flush=True,
                )
    finally:
        if not initially_loaded and not args.keep_module and module_loaded():
            cleanup = subprocess.run(
                ["rmmod", "mooncake_hbm_p2p_probe"], check=False
            )
            if cleanup.returncode != 0:
                print("[WARN] cleanup: failed to unload probe module", flush=True)

    summary = summarize(samples, args.sizes)
    for size, result in summary["by_size"].items():
        print(
            "MATRIX_SIZE "
            f"size={size} passed={result['passed']}/{result['attempts']} "
            f"page_count={result['page_count_min']}..{result['page_count_max']} "
            f"pa_runs={result['pa_runs_min']}..{result['pa_runs_max']} "
            f"dma_runs={result['dma_runs_min']}..{result['dma_runs_max']}",
            flush=True,
        )
    print(
        "MATRIX_RESULT "
        f"passed={summary['passed']}/{summary['attempts']} "
        f"status={'PASS' if summary['all_passed'] else 'FAIL'} "
        "level=L1_5_NVME_DMA_API_MAPPED",
        flush=True,
    )
    if args.output:
        write_jsonl(args.output, [*samples, summary])
        print(f"MATRIX_OUTPUT path={args.output}", flush=True)
    return 0 if summary["all_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
