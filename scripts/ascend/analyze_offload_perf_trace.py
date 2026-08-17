#!/usr/bin/env python3
"""Summarize MOONCAKE_OFFLOAD_PERF key-value trace lines."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


PREFIX = "MOONCAKE_OFFLOAD_PERF"
FIELD_RE = re.compile(r"([a-z_]+)=([^\s]+)")


def percentile(values: list[int], fraction: float) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


def parse_line(line: str) -> dict[str, Any] | None:
    marker = line.find(PREFIX)
    if marker < 0:
        return None
    fields = dict(FIELD_RE.findall(line[marker + len(PREFIX) :]))
    required = {"stage", "batch_id", "bytes", "duration_us", "success"}
    if not required.issubset(fields):
        return None
    for name in (
        "batch_id",
        "keys",
        "bytes",
        "duration_us",
        "end_steady_ns",
        "process_id",
        "thread_id",
        "success",
        "error_code",
    ):
        if name in fields:
            fields[name] = int(fields[name])
    return fields


def summarize(events: list[dict[str, Any]]) -> dict[str, Any]:
    by_stage: dict[str, list[dict[str, Any]]] = defaultdict(list)
    by_batch: dict[tuple[str, int], dict[str, dict[str, Any]]] = defaultdict(dict)
    for event in events:
        by_stage[event["stage"]].append(event)
        batch_id = event["batch_id"]
        endpoint = event.get("endpoint", "-")
        if batch_id > 0:
            by_batch[(endpoint, batch_id)][event["stage"]] = event

    stages: dict[str, Any] = {}
    for stage, stage_events in sorted(by_stage.items()):
        successful = [event for event in stage_events if event["success"] == 1]
        durations = [event["duration_us"] for event in successful]
        total_duration_us = sum(durations)
        total_bytes = sum(event["bytes"] for event in successful)
        stages[stage] = {
            "count": len(stage_events),
            "success_count": len(successful),
            "failure_count": len(stage_events) - len(successful),
            "bytes": total_bytes,
            "duration_us": {
                "avg": (sum(durations) / len(durations)) if durations else None,
                "p50": percentile(durations, 0.50),
                "p95": percentile(durations, 0.95),
                "p99": percentile(durations, 0.99),
            },
            "aggregate_gib_per_s": (
                total_bytes / total_duration_us * 1_000_000 / (1024**3)
                if total_duration_us > 0
                else None
            ),
        }

    opportunities = []
    for (endpoint, batch_id), stage_map in sorted(by_batch.items()):
        ssd = stage_map.get("ssd_to_staging")
        ssd_source = "ssd_to_staging"
        if ssd is None:
            ssd = stage_map.get("ssd_rpc_wait")
            ssd_source = "ssd_rpc_wait_fallback"
        hbm = stage_map.get("staging_to_hbm")
        total = stage_map.get("offload_get_total")
        if not ssd or not hbm or not total:
            continue
        if not (ssd["success"] and hbm["success"] and total["success"]):
            continue
        serial_stage_us = ssd["duration_us"] + hbm["duration_us"]
        ideal_pipeline_us = max(ssd["duration_us"], hbm["duration_us"])
        opportunities.append(
            {
                "endpoint": endpoint,
                "batch_id": batch_id,
                "bytes": total["bytes"],
                "ssd_stage_source": ssd_source,
                "ssd_stage_us": ssd["duration_us"],
                "staging_to_hbm_us": hbm["duration_us"],
                "measured_total_us": total["duration_us"],
                "serial_stage_us": serial_stage_us,
                "ideal_pipeline_us": ideal_pipeline_us,
                "max_hideable_us": min(ssd["duration_us"], hbm["duration_us"]),
                "stage_only_speedup_upper_bound": (
                    serial_stage_us / ideal_pipeline_us
                    if ideal_pipeline_us > 0
                    else None
                ),
            }
        )

    return {
        "schema_version": 1,
        "event_count": len(events),
        "stages": stages,
        "per_batch_overlap_upper_bound": opportunities,
        "notes": [
            "ssd_to_staging is preferred for the SSD stage when owner and "
            "requester logs are combined.",
            "ssd_rpc_wait is a fallback and includes owner allocation, SSD "
            "read, RPC, and scheduling overhead.",
            "staging_to_hbm is the blocking Mooncake transfer duration.",
            "Upper bounds are models, not measured pipeline results.",
        ],
    }


def _number(value: float | int | None, digits: int = 1) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def render_human(report: dict[str, Any]) -> str:
    lines = [
        "MOONCAKE SSD RESTORE BASELINE",
        f"events={report['event_count']}",
        "",
        f"{'stage':<24} {'ok/all':>9} {'p50_us':>10} {'p95_us':>10} "
        f"{'GiB/s':>10}",
    ]
    for stage, metrics in report["stages"].items():
        duration = metrics["duration_us"]
        lines.append(
            f"{stage:<24} "
            f"{metrics['success_count']}/{metrics['count']:>7} "
            f"{_number(duration['p50']):>10} "
            f"{_number(duration['p95']):>10} "
            f"{_number(metrics['aggregate_gib_per_s'], 3):>10}"
        )

    opportunities = report["per_batch_overlap_upper_bound"]
    lines.extend(["", "PIPELINE UPPER BOUND (model, not measured)"])
    if not opportunities:
        lines.append("no correlated successful SSD/HBM batches")
        return "\n".join(lines)

    serial_us = sum(item["serial_stage_us"] for item in opportunities)
    ideal_us = sum(item["ideal_pipeline_us"] for item in opportunities)
    hideable_us = sum(item["max_hideable_us"] for item in opportunities)
    fallback_count = sum(
        item["ssd_stage_source"] == "ssd_rpc_wait_fallback"
        for item in opportunities
    )
    lines.append(
        f"batches={len(opportunities)} serial_stage_us={serial_us} "
        f"ideal_overlap_us={ideal_us} hideable_us={hideable_us} "
        f"stage_speedup_upper={_number(serial_us / ideal_us if ideal_us else None, 3)}x"
    )
    if fallback_count:
        lines.append(
            f"WARNING: {fallback_count} batch(es) used ssd_rpc_wait; combine "
            "owner logs to remove RPC/allocation overhead from the SSD stage."
        )
    return "\n".join(lines)


def render_live_event(event: dict[str, Any]) -> str:
    duration_us = event["duration_us"]
    gib_per_s = (
        event["bytes"] / duration_us * 1_000_000 / (1024**3)
        if duration_us > 0
        else None
    )
    return (
        f"LIVE stage={event['stage']} batch={event['batch_id']} "
        f"bytes={event['bytes']} duration_us={duration_us} "
        f"GiB/s={_number(gib_per_s, 3)} success={event['success']} "
        f"error={event.get('error_code', '-')}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--format", choices=("human", "json", "both"), default="human"
    )
    parser.add_argument(
        "--live",
        action="store_true",
        help="print each parsed stage immediately; use log '-' with tail -F",
    )
    args = parser.parse_args()

    events = []
    try:
        for log_path in args.log:
            handle = (
                sys.stdin
                if str(log_path) == "-"
                else log_path.open("r", encoding="utf-8", errors="replace")
            )
            try:
                for line in handle:
                    event = parse_line(line)
                    if event is not None:
                        events.append(event)
                        if args.live:
                            print(render_live_event(event), flush=True)
            finally:
                if handle is not sys.stdin:
                    handle.close()
    except KeyboardInterrupt:
        print("\nLIVE collection stopped; printing accumulated summary.")
    report = summarize(events)
    rendered = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    if args.format in ("human", "both"):
        print(render_human(report))
    if args.format in ("json", "both"):
        print(rendered)
    if args.output:
        print(f"JSON report: {args.output.resolve()}")
    return 0 if events else 2


if __name__ == "__main__":
    raise SystemExit(main())
