import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "collect_a3_ssd_direct_env.py"
SPEC = importlib.util.spec_from_file_location("collect_a3_ssd_direct_env", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class CollectA3SsdDirectEnvTest(unittest.TestCase):
    def test_platform_verdict_never_claims_direct_dma(self):
        ok = {"returncode": 0, "stdout": "collected\n"}
        report = {
            "commands": {"pci_tree": ok, "pci_verbose": ok},
            "direct_storage_platform": {
                "kernel_config": {
                    "values": {"CONFIG_PCI_P2PDMA": "y"}
                },
                "storage_path": {"resolved": "/nvme", "findmnt": ok},
                "requested_npu_bdfs": ["0000:01:00.0"],
                "requested_nvme_bdfs": ["0000:02:00.0"],
                "pci_devices": [
                    {"role": "npu", "bdf": "0000:01:00.0", "lspci": ok},
                    {"role": "nvme", "bdf": "0000:02:00.0", "lspci": ok},
                ],
            },
        }
        verdict = MODULE.platform_verdict(report)
        self.assertTrue(verdict["evidence_complete"])
        self.assertTrue(verdict["p2pdma_kernel_enabled"])
        self.assertFalse(verdict["direct_dma_qualified"])

    def test_strict_identity_matches_complete_chain(self):
        report = {
            "package": {
                "version": "0.3.11.post1",
                "installed_native_files": [
                    {"path": "/site/mooncake/libx.so", "sha256": "abc"}
                ],
            },
            "source": {
                "head": {
                    "returncode": 0,
                    "stdout": MODULE.EXPECTED_COMMIT + "\n",
                }
            },
            "wheel_files": [
                {
                    "native_files": [
                        {
                            "basename": "libx.so",
                            "sha256": "abc",
                        }
                    ]
                }
            ],
            "loaded_native_files": [
                {
                    "pid": 7,
                    "files": [
                        {
                            "path": "/site/mooncake/libx.so",
                            "loaded_path": "/site/mooncake/libx.so",
                            "sha256": "abc",
                        }
                    ],
                }
            ],
        }
        verdict = MODULE.identity_verdict(
            report, MODULE.EXPECTED_VERSION, MODULE.EXPECTED_COMMIT
        )
        self.assertTrue(verdict["strict_identity_pass"])

    def test_strict_identity_fails_loaded_hash_mismatch(self):
        report = {
            "package": {
                "version": "0.3.11.post1",
                "installed_native_files": [
                    {"path": "/site/mooncake/libx.so", "sha256": "abc"}
                ],
            },
            "source": {
                "head": {
                    "returncode": 0,
                    "stdout": MODULE.EXPECTED_COMMIT + "\n",
                }
            },
            "wheel_files": [
                {
                    "native_files": [
                        {"basename": "libx.so", "sha256": "abc"}
                    ]
                }
            ],
            "loaded_native_files": [
                {
                    "pid": 7,
                    "files": [
                        {
                            "path": "/site/mooncake/libx.so",
                            "loaded_path": "/other/libx.so",
                            "sha256": "different",
                        }
                    ],
                }
            ],
        }
        verdict = MODULE.identity_verdict(
            report, MODULE.EXPECTED_VERSION, MODULE.EXPECTED_COMMIT
        )
        self.assertFalse(verdict["loaded_installed_native_match"])
        self.assertFalse(verdict["strict_identity_pass"])


if __name__ == "__main__":
    unittest.main()
