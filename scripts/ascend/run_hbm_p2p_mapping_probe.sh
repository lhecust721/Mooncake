#!/usr/bin/env bash
# Copyright 2026 KVCache.AI
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "${script_dir}/../.." && pwd)
module_path="${repo_root}/mooncake-transfer-engine/example/hbm_p2p_probe/mooncake_hbm_p2p_probe.ko"
binary_path=""
nvme_bdf=""
npu_device=0
probe_size=4194304
export_only=0
keep_module=0
loaded_by_script=0
created_device_node=0

usage() {
    cat <<'EOF'
usage: run_hbm_p2p_mapping_probe.sh --binary PATH
       (--nvme-bdf DDDD:BB:DD.F | --export-only)
       [--module PATH] [--device ID] [--size BYTES] [--keep-module]

Runs entirely from local files and prints PASS/FAIL plus a final RUNNER_RESULT.
It never submits an NVMe command or reads/writes the block device.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --binary)
            binary_path=${2:-}
            shift 2
            ;;
        --module)
            module_path=${2:-}
            shift 2
            ;;
        --nvme-bdf)
            nvme_bdf=${2:-}
            shift 2
            ;;
        --device)
            npu_device=${2:-}
            shift 2
            ;;
        --size)
            probe_size=${2:-}
            shift 2
            ;;
        --export-only)
            export_only=1
            shift
            ;;
        --keep-module)
            keep_module=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[FAIL] arguments: unknown option $1"
            usage
            exit 2
            ;;
    esac
done

cleanup() {
    if [ "${created_device_node}" -eq 1 ] &&
       [ -c /dev/mooncake_hbm_p2p_probe ]; then
        rm -f -- /dev/mooncake_hbm_p2p_probe ||
            echo "[WARN] cleanup: could not remove temporary device node"
    fi
    if [ "${loaded_by_script}" -eq 1 ] && [ "${keep_module}" -eq 0 ]; then
        if command -v rmmod >/dev/null 2>&1; then
            rmmod mooncake_hbm_p2p_probe ||
                echo "[WARN] cleanup: could not unload mooncake_hbm_p2p_probe"
        fi
    fi
    return 0
}
trap cleanup EXIT

if [ -z "${binary_path}" ] || [ ! -x "${binary_path}" ]; then
    echo "[FAIL] user_probe: executable not found: ${binary_path:-<empty>}"
    exit 2
fi
echo "[PASS] user_probe: ${binary_path}"

if grep -q '^drv_devmm_host ' /proc/modules; then
    echo "[PASS] ascend_driver: drv_devmm_host is loaded"
else
    echo "[FAIL] ascend_driver: drv_devmm_host is not loaded"
    exit 3
fi

if grep -q '__ksymtab_hal_kernel_p2p_get_pages' /proc/kallsyms &&
   grep -q '__ksymtab_hal_kernel_p2p_put_pages' /proc/kallsyms; then
    echo "[PASS] ascend_exports: HBM get/put page symbols are exported"
else
    echo "[FAIL] ascend_exports: HBM get/put page exports are unavailable"
    exit 3
fi

if [ "${export_only}" -eq 0 ]; then
    if [ -z "${nvme_bdf}" ] ||
       [ ! -d "/sys/bus/pci/devices/${nvme_bdf}" ]; then
        echo "[FAIL] nvme_bdf: PCI device not found: ${nvme_bdf:-<empty>}"
        exit 2
    fi
    pci_class=$(cat "/sys/bus/pci/devices/${nvme_bdf}/class" 2>/dev/null || true)
    case "${pci_class}" in
        0x0108*)
            echo "[PASS] nvme_bdf: ${nvme_bdf} class=${pci_class}"
            ;;
        *)
            echo "[FAIL] nvme_bdf: ${nvme_bdf} class=${pci_class:-unknown} is not NVMe"
            exit 2
            ;;
    esac
fi

if grep -q '^mooncake_hbm_p2p_probe ' /proc/modules; then
    echo "[PASS] kernel_probe: module already loaded"
else
    if [ ! -f "${module_path}" ]; then
        echo "[FAIL] kernel_probe: module not found: ${module_path}"
        exit 4
    fi
    if ! command -v insmod >/dev/null 2>&1; then
        echo "[FAIL] kernel_probe: insmod is missing; install/use local kmod on the host"
        exit 4
    fi
    echo "[INFO] kernel_probe: loading ${module_path}"
    if ! insmod "${module_path}"; then
        echo "[FAIL] kernel_probe: insmod failed; inspect dmesg for symbol/version errors"
        exit 4
    fi
    loaded_by_script=1
    echo "[PASS] kernel_probe: module loaded"
fi

if [ ! -c /dev/mooncake_hbm_p2p_probe ]; then
    misc_dev=/sys/class/misc/mooncake_hbm_p2p_probe/dev
    if [ -r "${misc_dev}" ] && command -v mknod >/dev/null 2>&1; then
        major_minor=$(cat "${misc_dev}")
        major_number=${major_minor%:*}
        minor_number=${major_minor#*:}
        mknod /dev/mooncake_hbm_p2p_probe c "${major_number}" "${minor_number}"
        chmod 600 /dev/mooncake_hbm_p2p_probe
        created_device_node=1
    fi
fi

if [ ! -c /dev/mooncake_hbm_p2p_probe ]; then
    echo "[FAIL] probe_device: /dev/mooncake_hbm_p2p_probe is unavailable"
    exit 4
fi
echo "[PASS] probe_device: /dev/mooncake_hbm_p2p_probe"

probe_args=(--device "${npu_device}" --size "${probe_size}")
if [ "${export_only}" -eq 1 ]; then
    probe_args+=(--export-only)
else
    probe_args+=(--nvme-bdf "${nvme_bdf}")
fi

"${binary_path}" "${probe_args[@]}"
probe_status=$?

if [ "${probe_status}" -eq 0 ]; then
    echo "RUNNER_RESULT probe_exit=0 requested_checks=PASS"
else
    echo "RUNNER_RESULT probe_exit=${probe_status} requested_checks=FAIL"
fi
exit "${probe_status}"
