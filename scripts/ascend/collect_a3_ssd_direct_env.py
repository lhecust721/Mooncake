#!/usr/bin/env python3
"""Collect A3/Mooncake identity and GDS-style direct-storage evidence."""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import hashlib
import importlib.metadata
import importlib.util
import json
import os
import platform
import socket
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any


PACKAGE = "mooncake-transfer-engine-npu"
EXPECTED_VERSION = "0.3.11.post1"
EXPECTED_COMMIT = "e9c61075720039bcfc5fffd19f847608402be3d0"
NATIVE_MARKERS = ("mooncake", "ascend", "acl", "hccl", "torch_npu", "adxl", "uring")
SAFE_ENV = (
    "ASCEND_HOME_PATH",
    "ASCEND_OPP_PATH",
    "ASCEND_RT_VISIBLE_DEVICES",
    "ASCEND_ENABLE_USE_FABRIC_MEM",
    "HCCL_INTRA_ROCE_ENABLE",
    "LD_LIBRARY_PATH",
    "MOONCAKE_CONFIG_PATH",
    "MOONCAKE_OFFLOAD_BUCKET_EVICTION_POLICY",
    "MOONCAKE_OFFLOAD_BUCKET_MAX_TOTAL_SIZE",
    "MOONCAKE_OFFLOAD_LOCAL_BUFFER_SIZE_BYTES",
    "MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES",
    "PYTHONHASHSEED",
)
VERSION_FILES = (
    "/usr/local/Ascend/ascend-toolkit/latest/version.cfg",
    "/usr/local/Ascend/driver/version.info",
    "/etc/ascend_install.info",
)
KERNEL_CONFIG_KEYS = (
    "CONFIG_PCI_P2PDMA",
    "CONFIG_IOMMU_SUPPORT",
    "CONFIG_IOMMU_DEFAULT_DMA_STRICT",
    "CONFIG_IOMMU_DEFAULT_PASSTHROUGH",
    "CONFIG_ARM_SMMU",
    "CONFIG_ARM_SMMU_V3",
    "CONFIG_VFIO",
    "CONFIG_VFIO_PCI",
)
MODULE_MARKERS = (
    "ascend",
    "davinci",
    "npu",
    "hisi",
    "vfio",
    "iommu",
    "nvme",
    "ub",
)


def sha256(path: Path) -> str | None:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def file_record(path: Path) -> dict[str, Any]:
    try:
        stat = path.stat()
        return {
            "path": str(path.resolve()),
            "size": stat.st_size,
            "sha256": sha256(path),
        }
    except OSError as error:
        return {"path": str(path), "error": str(error)}


def wheel_record(path: Path) -> dict[str, Any]:
    record = file_record(path)
    native_files = []
    try:
        with zipfile.ZipFile(path) as wheel:
            for member in wheel.infolist():
                name = member.filename
                if not (name.endswith(".so") or ".so." in Path(name).name):
                    continue
                digest = hashlib.sha256()
                with wheel.open(member) as handle:
                    for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                        digest.update(chunk)
                native_files.append(
                    {
                        "member": name,
                        "basename": Path(name).name,
                        "size": member.file_size,
                        "sha256": digest.hexdigest(),
                    }
                )
        record["native_files"] = native_files
    except (OSError, zipfile.BadZipFile) as error:
        record["wheel_error"] = str(error)
    return record


def run(command: list[str], cwd: Path | None = None) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=20,
            check=False,
        )
        return {
            "argv": command,
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"argv": command, "error": str(error)}


def read_text(path: Path) -> dict[str, Any]:
    try:
        return {
            "path": str(path),
            "text": path.read_text(encoding="utf-8", errors="replace"),
        }
    except OSError as error:
        return {"path": str(path), "error": str(error)}


def kernel_config() -> dict[str, Any]:
    candidates = [
        Path("/proc/config.gz"),
        Path("/boot") / f"config-{platform.release()}",
    ]
    for path in candidates:
        try:
            if path.suffix == ".gz":
                with gzip.open(
                    path, "rt", encoding="utf-8", errors="replace"
                ) as handle:
                    lines = handle.readlines()
            else:
                lines = path.read_text(
                    encoding="utf-8", errors="replace"
                ).splitlines()
        except OSError:
            continue
        values = {}
        for line in lines:
            for key in KERNEL_CONFIG_KEYS:
                if line.startswith(f"{key}="):
                    values[key] = line.split("=", 1)[1].strip()
                elif line == f"# {key} is not set":
                    values[key] = "n"
        return {"path": str(path), "values": values}
    return {"error": "kernel config not readable", "values": {}}


def relevant_modules() -> dict[str, Any]:
    modules = read_text(Path("/proc/modules"))
    if "text" not in modules:
        return modules
    selected = []
    for line in modules["text"].splitlines():
        name = line.split(maxsplit=1)[0]
        lowered = name.lower()
        if any(marker in lowered for marker in MODULE_MARKERS):
            selected.append(line)
    return {"path": modules["path"], "modules": selected}


def iommu_groups() -> dict[str, Any]:
    root = Path("/sys/kernel/iommu_groups")
    try:
        groups = []
        for group in sorted(root.iterdir(), key=lambda item: item.name):
            devices = group / "devices"
            groups.append(
                {
                    "group": group.name,
                    "devices": sorted(item.name for item in devices.iterdir()),
                }
            )
        return {"path": str(root), "count": len(groups), "groups": groups}
    except OSError as error:
        return {"path": str(root), "error": str(error)}


def pci_p2pdma_resources() -> list[dict[str, Any]]:
    records = []
    root = Path("/sys/bus/pci/devices")
    try:
        devices = list(root.iterdir())
    except OSError:
        return records
    for device in devices:
        p2pmem = device / "p2pmem"
        if p2pmem.exists():
            records.append({"bdf": device.name, "path": str(p2pmem)})
    return sorted(records, key=lambda item: item["bdf"])


def storage_path(path: Path | None) -> dict[str, Any]:
    if path is None:
        return {"error": "--ssd-path not supplied"}
    try:
        resolved = path.resolve(strict=True)
        record = {
            "requested": str(path),
            "resolved": str(resolved),
            "findmnt": run(["findmnt", "-J", "-T", str(resolved)]),
            "df_type": run(["df", "-T", str(resolved)]),
        }
        if hasattr(os, "statvfs"):
            stats = os.statvfs(resolved)
            record["filesystem_block_size"] = stats.f_bsize
            record["fragment_size"] = stats.f_frsize
        else:
            record["statvfs_error"] = "os.statvfs is unavailable"
        return record
    except OSError as error:
        return {"requested": str(path), "error": str(error)}


def pci_device_details(
    npu_bdfs: list[str], nvme_bdfs: list[str]
) -> list[dict[str, Any]]:
    records = []
    for role, bdfs in (("npu", npu_bdfs), ("nvme", nvme_bdfs)):
        for bdf in bdfs:
            records.append(
                {
                    "role": role,
                    "bdf": bdf,
                    "lspci": run(["lspci", "-D", "-s", bdf, "-vv"]),
                }
            )
    return records


def important_pci_lines(output: str) -> list[str]:
    markers = (
        "ACSCtl:",
        "IOMMU group:",
        "LnkCap:",
        "LnkSta:",
        "NUMA node:",
        "Physical Slot:",
    )
    lines = output.splitlines()
    selected = lines[:1]
    selected.extend(line.strip() for line in lines if any(m in line for m in markers))
    return selected


def mount_summary(storage: dict[str, Any]) -> str:
    output = command_stdout(storage.get("findmnt", {}))
    if output is None:
        return "unavailable"
    try:
        filesystems = json.loads(output).get("filesystems", [])
        mount = filesystems[0]
        return (
            f"source={mount.get('source', '-')} target={mount.get('target', '-')} "
            f"fstype={mount.get('fstype', '-')} options={mount.get('options', '-')}"
        )
    except (json.JSONDecodeError, IndexError, KeyError, TypeError):
        return output.splitlines()[0] if output else "unavailable"


def package_identity(package: str) -> dict[str, Any]:
    result: dict[str, Any] = {
        "pip_show": run([sys.executable, "-m", "pip", "show", package]),
        "pip_show_files": run(
            [sys.executable, "-m", "pip", "show", "--files", package]
        ),
    }
    try:
        distribution = importlib.metadata.distribution(package)
    except importlib.metadata.PackageNotFoundError as error:
        result["metadata_error"] = str(error)
        return result

    result["name"] = distribution.metadata.get("Name")
    result["version"] = distribution.version
    result["install_root"] = str(Path(distribution.locate_file("")).resolve())
    native_files = []
    for relative in distribution.files or []:
        installed = Path(distribution.locate_file(relative))
        if (
            installed.name.endswith((".so", ".so.0", ".so.1"))
            or ".so." in installed.name
        ):
            native_files.append(file_record(installed))
        if relative.name == "RECORD":
            result["record"] = file_record(installed)
    result["installed_native_files"] = native_files

    modules = {}
    for module in ("mooncake", "mooncake_store", "transfer_engine"):
        try:
            spec = importlib.util.find_spec(module)
            modules[module] = None if spec is None else {
                "origin": spec.origin,
                "locations": list(spec.submodule_search_locations or []),
            }
        except (ImportError, AttributeError, ValueError) as error:
            modules[module] = {"error": str(error)}
    result["module_specs"] = modules
    return result


def loaded_native_files(pid: int) -> dict[str, Any]:
    maps = Path(f"/proc/{pid}/maps")
    try:
        paths: dict[str, Path] = {}
        for line in maps.read_text(encoding="utf-8", errors="replace").splitlines():
            fields = line.split(maxsplit=5)
            if len(fields) < 6 or not fields[5].startswith("/"):
                continue
            loaded_path = fields[5]
            path = Path(loaded_path.removesuffix(" (deleted)"))
            lowered = path.name.lower()
            if ".so" in lowered and any(marker in lowered for marker in NATIVE_MARKERS):
                paths[loaded_path] = path
        files = []
        for loaded_path, path in sorted(paths.items()):
            record = file_record(path)
            record["loaded_path"] = loaded_path
            record["deleted_mapping"] = loaded_path.endswith(" (deleted)")
            files.append(record)
        return {"pid": pid, "maps": str(maps), "files": files}
    except OSError as error:
        return {"pid": pid, "maps": str(maps), "error": str(error)}


def git_identity(repo: Path) -> dict[str, Any]:
    return {
        "head": run(["git", "rev-parse", "HEAD"], repo),
        "branch": run(["git", "branch", "--show-current"], repo),
        "status": run(["git", "status", "--short"], repo),
        "describe": run(["git", "describe", "--tags", "--always", "--dirty"], repo),
    }


def command_stdout(command: dict[str, Any]) -> str | None:
    if command.get("returncode") != 0:
        return None
    return command.get("stdout", "").strip()


def identity_verdict(
    report: dict[str, Any], expected_version: str, expected_commit: str
) -> dict[str, Any]:
    package = report["package"]
    source = report["source"]
    installed = package.get("installed_native_files", [])
    wheel_native = [
        native
        for wheel in report["wheel_files"]
        for native in wheel.get("native_files", [])
    ]
    wheel_by_name: dict[str, set[str]] = {}
    for native in wheel_native:
        wheel_by_name.setdefault(native["basename"], set()).add(native["sha256"])
    installed_by_name = {
        Path(native["path"]).name: native.get("sha256") for native in installed
    }
    unmatched_installed = [
        native["path"]
        for native in installed
        if Path(native["path"]).name in wheel_by_name
        and native.get("sha256") not in wheel_by_name[Path(native["path"]).name]
    ]
    missing_installed = sorted(set(wheel_by_name) - set(installed_by_name))
    loaded_sets = report["loaded_native_files"]
    loaded_package_files = [
        native
        for process in loaded_sets
        for native in process.get("files", [])
        if Path(native["path"]).name in installed_by_name
    ]
    loaded_mismatches = [
        native["loaded_path"]
        for native in loaded_package_files
        if native.get("sha256") != installed_by_name[Path(native["path"]).name]
    ]
    wheel_comparison_available = bool(
        wheel_native and "metadata_error" not in package
    )
    verdict = {
        "package_version_match": package.get("version") == expected_version,
        "source_commit_match": command_stdout(source["head"]) == expected_commit,
        "wheel_native_comparison_available": wheel_comparison_available,
        "wheel_installed_native_match": (
            not unmatched_installed and not missing_installed
            if wheel_comparison_available
            else None
        ),
        "unmatched_installed_native_files": unmatched_installed,
        "wheel_native_missing_from_install": missing_installed,
        "loaded_installed_native_match": (
            not loaded_mismatches if loaded_package_files else None
        ),
        "loaded_installed_native_mismatches": loaded_mismatches,
        "requested_pid_count": len(loaded_sets),
        "pid_maps_readable_count": sum("error" not in item for item in loaded_sets),
        "loaded_native_file_count": sum(
            len(item.get("files", [])) for item in loaded_sets
        ),
    }
    verdict["strict_identity_pass"] = all(
        (
            verdict["package_version_match"],
            verdict["source_commit_match"],
            verdict["wheel_installed_native_match"] is True,
            verdict["requested_pid_count"] > 0,
            verdict["pid_maps_readable_count"]
            == verdict["requested_pid_count"],
            verdict["loaded_native_file_count"] > 0,
            verdict["loaded_installed_native_match"] is True,
        )
    )
    return verdict


def platform_verdict(report: dict[str, Any]) -> dict[str, Any]:
    platform_evidence = report["direct_storage_platform"]
    kernel_values = platform_evidence["kernel_config"].get("values", {})
    storage = platform_evidence["storage_path"]
    commands = report["commands"]
    checks = {
        "ssd_path_resolved": "resolved" in storage,
        "mount_identified": command_stdout(storage.get("findmnt", {}))
        is not None,
        "pci_topology_collected": command_stdout(commands["pci_tree"])
        is not None,
        "pci_verbose_collected": command_stdout(commands["pci_verbose"])
        is not None,
        "kernel_config_collected": bool(kernel_values),
        "p2pdma_kernel_enabled": kernel_values.get("CONFIG_PCI_P2PDMA")
        in ("y", "m"),
        "npu_bdf_supplied": bool(platform_evidence["requested_npu_bdfs"]),
        "nvme_bdf_supplied": bool(platform_evidence["requested_nvme_bdfs"]),
        "pci_device_details_collected": bool(platform_evidence["pci_devices"])
        and all(
            command_stdout(device["lspci"]) is not None
            for device in platform_evidence["pci_devices"]
        ),
    }
    checks["evidence_complete"] = all(
        value for name, value in checks.items() if name != "p2pdma_kernel_enabled"
    )
    checks["direct_dma_qualified"] = False
    checks["qualification_note"] = (
        "Collection completeness is not proof of an NPU peer-DMA mapping API, "
        "filesystem/block-layer support, or physical NVMe-to-HBM DMA."
    )
    return checks


def render_human(report: dict[str, Any]) -> str:
    verdict = report["identity_verdict"]
    package = report["package"]
    source = report["source"]

    def state(value: bool | None) -> str:
        return "PASS" if value is True else "FAIL" if value is False else "N/A"

    lines = [
        "MOONCAKE A3 RUNTIME IDENTITY",
        f"overall_strict_identity [{state(verdict['strict_identity_pass'])}]",
        f"source_commit [{state(verdict['source_commit_match'])}] "
        f"actual={command_stdout(source['head']) or '-'} "
        f"expected={report['expected']['commit']}",
        f"package_version [{state(verdict['package_version_match'])}] "
        f"actual={package.get('version', '-')} "
        f"expected={report['expected']['version']}",
    ]
    if "metadata_error" in package:
        lines.append(f"package_error: {package['metadata_error']}")

    lines.append("WHEELS")
    if not report["wheel_files"]:
        lines.append("  WARNING: no --wheel supplied; artifact identity incomplete")
    for wheel in report["wheel_files"]:
        lines.append(
            f"  {wheel.get('path')} sha256={wheel.get('sha256', '-')} "
            f"native_files={len(wheel.get('native_files', []))}"
        )
    lines.append(
        "wheel_vs_installed_native "
        f"[{state(verdict['wheel_installed_native_match'])}]"
    )
    for path in verdict["unmatched_installed_native_files"]:
        lines.append(f"  MISMATCH: {path}")
    for name in verdict["wheel_native_missing_from_install"]:
        lines.append(f"  MISSING_FROM_INSTALL: {name}")

    lines.append("LOADED NATIVE LIBRARIES")
    if not report["loaded_native_files"]:
        lines.append("  WARNING: no --pid supplied; loaded .so identity incomplete")
    for process in report["loaded_native_files"]:
        if "error" in process:
            lines.append(f"  pid={process['pid']} ERROR: {process['error']}")
            continue
        lines.append(f"  pid={process['pid']} files={len(process['files'])}")
        for native in process["files"]:
            deleted = " DELETED" if native.get("deleted_mapping") else ""
            lines.append(
                f"    {native['loaded_path']}{deleted} "
                f"sha256={native.get('sha256', '-')}"
            )
    lines.append(
        "loaded_vs_installed_package_native "
        f"[{state(verdict['loaded_installed_native_match'])}]"
    )
    for path in verdict["loaded_installed_native_mismatches"]:
        lines.append(f"  MISMATCH: {path}")

    platform_check = report["platform_verdict"]
    platform_evidence = report["direct_storage_platform"]
    kernel_values = platform_evidence["kernel_config"].get("values", {})
    cmdline = platform_evidence["proc_cmdline"].get("text", "").strip()
    lines.extend(
        [
            "DIRECT-STORAGE PLATFORM EVIDENCE",
            "evidence_complete "
            f"[{state(platform_check['evidence_complete'])}]",
            "direct_dma_qualified [NO] (requires vendor/driver mapping proof)",
            f"CONFIG_PCI_P2PDMA="
            f"{kernel_values.get('CONFIG_PCI_P2PDMA', 'unknown')}",
            f"iommu_groups={platform_evidence['iommu_groups'].get('count', '-')}",
            f"kernel_cmdline={cmdline or '-'}",
            f"ssd_path={platform_evidence['storage_path'].get('resolved', '-')} ",
            f"ssd_mount={mount_summary(platform_evidence['storage_path'])}",
            f"npu_bdfs={platform_evidence['requested_npu_bdfs'] or '-'}",
            f"nvme_bdfs={platform_evidence['requested_nvme_bdfs'] or '-'}",
            "kernel_config:",
        ]
    )
    for key in KERNEL_CONFIG_KEYS:
        lines.append(f"  {key}={kernel_values.get(key, 'unknown')}")
    lines.append(
        "pci_p2pdma_resources="
        f"{platform_evidence['pci_p2pdma_resources'] or '-'}"
    )
    lines.extend(
        [
            "selected_pci_devices:",
        ]
    )
    for device in platform_evidence["pci_devices"]:
        output = command_stdout(device["lspci"])
        if output is None:
            lines.append(f"  {device['role']} {device['bdf']} ERROR")
            continue
        lines.append(f"  {device['role']} {device['bdf']}")
        for detail in important_pci_lines(output):
            lines.append(f"    {detail}")
    lines.extend(
        [
            "relevant_modules:",
        ]
    )
    for module in platform_evidence["relevant_modules"].get("modules", []):
        lines.append(f"  {module}")
    if not platform_evidence["relevant_modules"].get("modules"):
        lines.append("  WARNING: no matching modules collected")
    non_required = {
        "direct_dma_qualified",
        "evidence_complete",
        "p2pdma_kernel_enabled",
    }
    for name, passed in platform_check.items():
        if isinstance(passed, bool) and not passed and name not in non_required:
            lines.append(f"  INCOMPLETE: {name}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pid", type=int, action="append", default=[])
    parser.add_argument("--wheel", type=Path, action="append", default=[])
    parser.add_argument("--ssd-path", type=Path)
    parser.add_argument("--npu-bdf", action="append", default=[])
    parser.add_argument("--nvme-bdf", action="append", default=[])
    parser.add_argument("--package", default=PACKAGE)
    parser.add_argument("--expected-version", default=EXPECTED_VERSION)
    parser.add_argument("--expected-commit", default=EXPECTED_COMMIT)
    parser.add_argument(
        "--strict",
        action="store_true",
        help="return exit code 3 when the runtime identity chain is incomplete",
    )
    parser.add_argument(
        "--strict-platform",
        action="store_true",
        help="return exit code 4 when platform evidence is incomplete",
    )
    parser.add_argument(
        "--repo", type=Path, default=Path(__file__).resolve().parents[2]
    )
    args = parser.parse_args()

    commands = {
        "uname": run(["uname", "-a"]),
        "glibc": run(["ldd", "--version"]),
        "npu_smi": run(["npu-smi", "info"]),
        "pci": run(["lspci", "-D", "-nn"]),
        "pci_tree": run(["lspci", "-D", "-t"]),
        "pci_verbose": run(["lspci", "-D", "-vv"]),
        "numa": run(["numactl", "--hardware"]),
        "block_devices": run(
            [
                "lsblk",
                "-J",
                "-b",
                "-o",
                "NAME,KNAME,PKNAME,PATH,MODEL,SERIAL,SIZE,ROTA,TYPE,FSTYPE,"
                "MOUNTPOINTS,LOG-SEC,PHY-SEC,MIN-IO,OPT-IO,ALIGNMENT",
            ]
        ),
        "mounts": run(["findmnt", "-J"]),
        "nvme": run(["nvme", "list", "-o", "json"]),
        "nvme_verbose": run(["nvme", "list", "-v", "-o", "json"]),
        "nvme_subsystems": run(["nvme", "list-subsys", "-o", "json"]),
    }
    version_files = {}
    for name in VERSION_FILES:
        path = Path(name)
        try:
            version_files[name] = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            version_files[name] = {"error": str(error)}

    report: dict[str, Any] = {
        "schema_version": 2,
        "collected_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "python": sys.version,
            "python_executable": sys.executable,
            "uid": os.getuid() if hasattr(os, "getuid") else None,
        },
        "environment": {name: os.environ.get(name) for name in SAFE_ENV},
        "commands": commands,
        "version_files": version_files,
        "expected": {
            "version": args.expected_version,
            "commit": args.expected_commit,
        },
        "package": package_identity(args.package),
        "wheel_files": [wheel_record(path) for path in args.wheel],
        "loaded_native_files": [loaded_native_files(pid) for pid in args.pid],
        "source": git_identity(args.repo.resolve()),
        "direct_storage_platform": {
            "requested_npu_bdfs": args.npu_bdf,
            "requested_nvme_bdfs": args.nvme_bdf,
            "pci_devices": pci_device_details(args.npu_bdf, args.nvme_bdf),
            "storage_path": storage_path(args.ssd_path),
            "proc_cmdline": read_text(Path("/proc/cmdline")),
            "kernel_config": kernel_config(),
            "relevant_modules": relevant_modules(),
            "iommu_groups": iommu_groups(),
            "pci_p2pdma_resources": pci_p2pdma_resources(),
        },
        "notes": [
            "Only an allowlist of non-secret environment variables is collected.",
            "Loaded .so evidence comes from /proc/PID/maps and should be "
            "captured while the target process is serving.",
            "A wheel hash identifies the artifact; installed native-file "
            "hashes identify what is present on disk; /proc maps identifies "
            "what is loaded.",
            "CONFIG_PCI_P2PDMA, ACS, IOMMU, or a shared PCIe root complex "
            "alone does not prove that Ascend HBM pages can be registered "
            "for storage DMA.",
        ],
    }
    report["identity_verdict"] = identity_verdict(
        report, args.expected_version, args.expected_commit
    )
    report["platform_verdict"] = platform_verdict(report)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(render_human(report))
    print(f"JSON report: {args.output.resolve()}")
    verdict = report["identity_verdict"]
    if args.strict and not verdict["strict_identity_pass"]:
        return 3
    if args.strict_platform and not report["platform_verdict"][
        "evidence_complete"
    ]:
        return 4
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
