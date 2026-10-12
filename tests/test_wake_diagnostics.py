"""Offline tests of diagnostic evidence boundaries; not acoustic acceptance."""
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("wake_diagnostics", Path(__file__).resolve().parents[1] / "scripts/diagnose_wake.py")
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class WakeDiagnosticsTests(unittest.TestCase):
    def test_private_audio_text_keys_and_logs_are_not_retained(self):
        for event in (None, [], {"type": []}, {"type": "input.audio", "audio": "private"},
                      {"type": "transcript.final", "text": "private"},
                      {"type": "device.status", "kind": "log", "message": "private"}):
            self.assertIsNone(diagnostics.sanitize(event))
        event = diagnostics.sanitize({"type": "device.recognition_debug", "phase": "timeout",
                                      "text": "private", "raw_text": "private", "token": "private",
                                      "matches": 0, "probability": float("nan")})
        self.assertEqual(event, {"type": "device.recognition_debug", "phase": "timeout", "matches": 0})
        self.assertFalse(diagnostics.numeric(10**400))

    def test_host_wake_and_model_timeout_do_not_count_as_keyword_match(self):
        report = diagnostics.summarize([
            {"type": "device.status", "kind": "wake", "message": "host_test"},
            {"type": "device.prompt", "name": "wake", "state": "completed"},
            {"type": "device.recognition_debug", "phase": "timeout", "matches": 1, "command_id": 1},
            {"type": "device.recognition", "matches": 0, "command_id": 1},
            {"type": "device.recognition", "matches": 1, "command_id": 2},
        ])
        self.assertEqual(report["local_keyword_matches"], 0)
        self.assertEqual(report["host_test_wakes"], 1)
        self.assertEqual(report["prompt_completions"], {"wake": 1})
        self.assertNotIn("failure_rate", report)

    def test_recognition_and_prompt_are_separate_observations(self):
        report = diagnostics.summarize([
            {"type": "device.recognition", "matches": 1, "command_id": 1, "probability": .25},
            {"type": "device.status", "kind": "wake_armed", "message": "private"},
            {"type": "device.prompt", "name": "sleep", "state": "completed"},
            {"type": "device.telemetry", "state": 2, "mn_detections": 99},
        ])
        self.assertEqual(report["local_keyword_matches"], 1)
        self.assertEqual(report["match_probabilities"], [.25])
        self.assertEqual(report["prompt_completions"], {"sleep": 1})
        self.assertEqual(report["latest_state"], "session_open_press_BOOT")
        self.assertEqual(report["wake_rearmed_events"], 1)
        self.assertNotIn("private", json.dumps(report))

    def test_sanitizing_again_preserves_safe_source(self):
        clean = diagnostics.sanitize({"type": "device.status", "kind": "wake", "message": "host_test"})
        self.assertEqual(diagnostics.sanitize(clean), clean)
        self.assertEqual(diagnostics.summarize([clean])["host_test_wakes"], 1)

    def test_idle_looking_states_are_distinguished_without_claiming_armed(self):
        sleep = diagnostics.summarize([{"type": "device.telemetry", "state": 0}])
        listening = diagnostics.summarize([{"type": "device.telemetry", "state": 2}])
        self.assertNotEqual(sleep["latest_state"], listening["latest_state"])
        self.assertEqual(sleep["wake_rearmed_events"], 0)
        self.assertEqual(diagnostics.summarize([])["latest_state"], "unknown")


if __name__ == "__main__":
    unittest.main()
