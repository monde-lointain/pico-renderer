#!/usr/bin/env python3
"""run_on_target_test.py — unit tests for the pure parse/assert core of the
on-target serial runner. No hardware, no picotool: feeds canned KEY=VALUE
streams to the tested core and asserts parse + pass/fail behavior.

Run standalone:  python3 tools/run_on_target_test.py
"""

import unittest

import run_on_target as rot


# A canned KEY=VALUE stream as the device would emit it over USB-CDC.
# Mixed: blank lines, a comment-ish noise line, ints, floats, the sentinel.
CANNED_STREAM = """\
PROBE_START
frame_us=12000
fps=83.3
tris=512
dropped=0
boot_msg=hello world
PROBE_DONE
"""


class TestParse(unittest.TestCase):
    def test_parses_key_value_pairs(self):
        result = rot.parse_stream(CANNED_STREAM.splitlines())
        self.assertEqual(result["frame_us"], "12000")
        self.assertEqual(result["fps"], "83.3")
        self.assertEqual(result["tris"], "512")
        self.assertEqual(result["dropped"], "0")

    def test_value_may_contain_spaces_and_equals(self):
        result = rot.parse_stream(["k=a = b"])
        self.assertEqual(result["k"], "a = b")

    def test_ignores_blank_and_non_kv_lines(self):
        result = rot.parse_stream(["", "   ", "PROBE_START", "x=1", "noise"])
        self.assertEqual(result, {"x": "1"})

    def test_strips_whitespace_around_key_and_value(self):
        result = rot.parse_stream(["  key  =  value  "])
        self.assertEqual(result["key"], "value")

    def test_last_value_wins_on_duplicate_key(self):
        result = rot.parse_stream(["x=1", "x=2"])
        self.assertEqual(result["x"], "2")


class TestSentinel(unittest.TestCase):
    def test_read_until_sentinel_stops_at_done(self):
        lines = ["a=1", "PROBE_DONE", "a=2"]
        collected = list(rot.read_until_sentinel(iter(lines), "PROBE_DONE"))
        self.assertEqual(collected, ["a=1"])

    def test_read_until_sentinel_yields_all_if_absent(self):
        lines = ["a=1", "b=2"]
        collected = list(rot.read_until_sentinel(iter(lines), "PROBE_DONE"))
        self.assertEqual(collected, ["a=1", "b=2"])


class TestThresholdAssert(unittest.TestCase):
    def test_le_passes_under_threshold(self):
        parsed = {"frame_us": "12000"}
        checks = [rot.Check("frame_us", "<=", 16667.0)]
        failures = rot.assert_thresholds(parsed, checks)
        self.assertEqual(failures, [])

    def test_le_fails_over_threshold(self):
        parsed = {"frame_us": "20000"}
        checks = [rot.Check("frame_us", "<=", 16667.0)]
        failures = rot.assert_thresholds(parsed, checks)
        self.assertEqual(len(failures), 1)
        self.assertIn("frame_us", failures[0])

    def test_ge_and_eq_and_lt_gt(self):
        parsed = {"fps": "83.3", "tris": "512", "dropped": "0"}
        checks = [
            rot.Check("fps", ">=", 60.0),
            rot.Check("tris", "==", 512.0),
            rot.Check("dropped", "<", 1.0),
            rot.Check("fps", ">", 83.0),
        ]
        self.assertEqual(rot.assert_thresholds(parsed, checks), [])

    def test_missing_key_is_a_failure(self):
        parsed = {"fps": "60"}
        checks = [rot.Check("frame_us", "<=", 16667.0)]
        failures = rot.assert_thresholds(parsed, checks)
        self.assertEqual(len(failures), 1)
        self.assertIn("missing", failures[0].lower())

    def test_non_numeric_value_is_a_failure(self):
        parsed = {"frame_us": "fast"}
        checks = [rot.Check("frame_us", "<=", 16667.0)]
        failures = rot.assert_thresholds(parsed, checks)
        self.assertEqual(len(failures), 1)
        self.assertIn("non-numeric", failures[0].lower())

    def test_multiple_failures_all_reported(self):
        parsed = {"fps": "30", "dropped": "5"}
        checks = [
            rot.Check("fps", ">=", 60.0),
            rot.Check("dropped", "<", 1.0),
        ]
        failures = rot.assert_thresholds(parsed, checks)
        self.assertEqual(len(failures), 2)


class TestParseCheckSpec(unittest.TestCase):
    def test_parse_check_spec_variants(self):
        self.assertEqual(
            rot.parse_check_spec("frame_us<=16667"),
            rot.Check("frame_us", "<=", 16667.0),
        )
        self.assertEqual(
            rot.parse_check_spec("fps>=60.0"),
            rot.Check("fps", ">=", 60.0),
        )
        self.assertEqual(
            rot.parse_check_spec("dropped==0"),
            rot.Check("dropped", "==", 0.0),
        )

    def test_parse_check_spec_rejects_garbage(self):
        with self.assertRaises(ValueError):
            rot.parse_check_spec("nonsense")
        with self.assertRaises(ValueError):
            rot.parse_check_spec("x~=5")


class TestEndToEndCore(unittest.TestCase):
    def test_canned_stream_passes_realistic_gate(self):
        parsed = rot.parse_stream(CANNED_STREAM.splitlines())
        checks = [
            rot.parse_check_spec("frame_us<=16667"),
            rot.parse_check_spec("fps>=60"),
            rot.parse_check_spec("dropped==0"),
        ]
        self.assertEqual(rot.assert_thresholds(parsed, checks), [])

    def test_canned_stream_fails_too_strict_gate(self):
        parsed = rot.parse_stream(CANNED_STREAM.splitlines())
        checks = [rot.parse_check_spec("frame_us<=10000")]
        self.assertNotEqual(rot.assert_thresholds(parsed, checks), [])


if __name__ == "__main__":
    unittest.main()
