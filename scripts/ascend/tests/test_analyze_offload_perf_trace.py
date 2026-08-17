import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "analyze_offload_perf_trace.py"
SPEC = importlib.util.spec_from_file_location("analyze_offload_perf_trace", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class AnalyzeOffloadPerfTraceTest(unittest.TestCase):
    def test_parse_and_overlap_upper_bound(self):
        lines = [
            "I x MOONCAKE_OFFLOAD_PERF stage=ssd_to_staging batch_id=7 keys=2 "
            "bytes=1048576 duration_us=700 end_steady_ns=1 thread_id=4 "
            "success=1 error_code=0 endpoint=127.0.0.1:1234",
            "I x MOONCAKE_OFFLOAD_PERF stage=ssd_rpc_wait batch_id=7 keys=2 "
            "bytes=1048576 duration_us=800 end_steady_ns=1 thread_id=3 "
            "success=1 error_code=0 endpoint=127.0.0.1:1234",
            "I x MOONCAKE_OFFLOAD_PERF stage=staging_to_hbm batch_id=7 keys=2 "
            "bytes=1048576 duration_us=200 end_steady_ns=2 thread_id=3 "
            "success=1 error_code=0 endpoint=127.0.0.1:1234",
            "I x MOONCAKE_OFFLOAD_PERF stage=offload_get_total batch_id=7 keys=2 "
            "bytes=1048576 duration_us=1050 end_steady_ns=3 thread_id=3 "
            "success=1 error_code=0 endpoint=127.0.0.1:1234",
        ]
        events = [MODULE.parse_line(line) for line in lines]
        report = MODULE.summarize([event for event in events if event])
        bound = report["per_batch_overlap_upper_bound"][0]
        self.assertEqual(bound["max_hideable_us"], 200)
        self.assertEqual(bound["ideal_pipeline_us"], 700)
        self.assertEqual(bound["stage_only_speedup_upper_bound"], 900 / 700)
        self.assertEqual(bound["ssd_stage_source"], "ssd_to_staging")

    def test_ignores_unrelated_and_malformed_lines(self):
        self.assertIsNone(MODULE.parse_line("normal log line"))
        self.assertIsNone(MODULE.parse_line("MOONCAKE_OFFLOAD_PERF stage=x"))

    def test_live_event_is_immediately_readable(self):
        event = MODULE.parse_line(
            "MOONCAKE_OFFLOAD_PERF stage=ssd_to_staging batch_id=2 keys=1 "
            "bytes=1073741824 duration_us=1000000 success=1 endpoint=x"
        )
        rendered = MODULE.render_live_event(event)
        self.assertIn("stage=ssd_to_staging", rendered)
        self.assertIn("GiB/s=1.000", rendered)

    def test_uses_rpc_wait_when_owner_log_is_absent(self):
        template = (
            "MOONCAKE_OFFLOAD_PERF stage={stage} batch_id=9 keys=1 bytes=10 "
            "duration_us={duration} success=1 endpoint=owner:1"
        )
        events = [
            MODULE.parse_line(template.format(stage="ssd_rpc_wait", duration=8)),
            MODULE.parse_line(
                template.format(stage="staging_to_hbm", duration=2)
            ),
            MODULE.parse_line(
                template.format(stage="offload_get_total", duration=11)
            ),
        ]
        report = MODULE.summarize([event for event in events if event])
        bound = report["per_batch_overlap_upper_bound"][0]
        self.assertEqual(bound["ssd_stage_source"], "ssd_rpc_wait_fallback")


if __name__ == "__main__":
    unittest.main()
