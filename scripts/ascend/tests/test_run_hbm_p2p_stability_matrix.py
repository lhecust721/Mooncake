import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).parents[1] / "run_hbm_p2p_stability_matrix.py"
SPEC = importlib.util.spec_from_file_location("hbm_matrix", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class HbmP2pStabilityMatrixTest(unittest.TestCase):
    def test_parse_successful_sample(self):
        sample = MODULE.parse_sample(
            [
                "[PASS] hbm_page_export: status=0 table_version=1 "
                "page_size=2097152 page_count=2 pa_runs=2 "
                "first_pa=0x1000 last_pa=0x3000 invalidated=0",
                "[PASS] nvme_dma_api_map: status=0 mapped=2/2 "
                "dma_runs=2 first_iova=0x1000 last_iova=0x3000 aligned=1",
                "RESULT hbm_export=PASS nvme_device=PASS "
                "nvme_dma_api_map=PASS pages_released=PASS "
                "level=L1_5_NVME_DMA_API_MAPPED",
            ]
        )
        self.assertTrue(sample["passed"])
        self.assertEqual(sample["page_count"], 2)
        self.assertEqual(sample["mapped_count"], 2)
        self.assertEqual(sample["dma_runs"], 2)

    def test_summary_keeps_fragmentation_ranges(self):
        samples = [
            {"size": 4096, "passed": True, "page_count": 1, "pa_runs": 1, "dma_runs": 1},
            {"size": 4096, "passed": True, "page_count": 1, "pa_runs": 2, "dma_runs": 2},
        ]
        summary = MODULE.summarize(samples, [4096])
        self.assertTrue(summary["all_passed"])
        self.assertEqual(summary["by_size"]["4096"]["pa_runs_max"], 2)


if __name__ == "__main__":
    unittest.main()
