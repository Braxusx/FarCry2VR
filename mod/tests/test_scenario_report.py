"""Synthetic file fixtures. Never game, GPU or headset evidence."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from verify_scenario_run import EXPECTED, Invalid, compare, verify


def fixture(directory, case="tracking-loss"):
    bmp = bytearray(58)
    bmp[:2] = b"BM"
    struct.pack_into("<I", bmp, 2, len(bmp))
    struct.pack_into("<IIiiHH", bmp, 10, 54, 40, 1, 1, 1, 24)
    rows = [{"event": "begin", "version": "scenario-runner-1", "case": case,
             "environment": "harness", "synthetic_input": True, "headset_accepted": False}]
    for index, name in EXPECTED[case]:
        seq, ticket = index + 100, 65536 | (index + 1)
        rows.append({"event": "input", "sequence": seq, "step": name, "ticket": ticket})
        for eye in (0, 1):
            file = f"step-{ticket}-frame-{seq}-eye{eye}.bmp"
            (directory / file).write_bytes(bmp)
            r = {"event": "capture", "ticket": ticket, "step": name, "file": file,
                 "sha256": hashlib.sha256(bmp).hexdigest(), "eye": eye, "app_frame": seq, "sequence": seq,
                 "first_sequence": seq, "owner": 1, "weapon": 10, "pose_sequence": seq, "pose_owner": 1,
                 "pose_app_frame": seq, "pose_physical": 1, "polled_sequence": seq, "polled_owner": 1,
                 "trigger": 0, "tracked": 1, "authority": 1, "hand_epoch": 1}
            if case == "weapon-swap" and index:
                r.update(owner=2, weapon=20, pose_owner=2, polled_owner=2)
            if name == "right-tracking-lost":
                r.update(tracked=0, authority=0, hand_epoch=2, pose_physical=0)
            if name == "tracked-trigger-held":
                r.update(authority=0, hand_epoch=2, pose_physical=0)
            if name == "neutral-rearm":
                r["hand_epoch"] = 2
            rows.append(r)
            rows.append({"event": "palette", "ticket": ticket, "eye": eye, "app_frame": seq,
                         "sequence": seq, "indices": list(range(6)), "model": [0.0] * 96, "world": [0.0] * 16})
    rows.append({"event": "end", "case": case, "observed_complete": True,
                 "reason": "complete-observed", "headset_accepted": False})
    save(directory, rows)
    return rows


def save(directory, rows):
    (directory / "trace.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")


class Reports(unittest.TestCase):
    def test_good_harness_controls(self):
        for case in EXPECTED:
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                fixture(root, case)
                report = verify(root)
                self.assertEqual(report["environment"], "harness")
                self.assertFalse(report["headset_accepted"])
                self.assertEqual(report["visual_review"], "required")
                self.assertEqual(report["stereo_pairs"], len(EXPECTED[case]))

    def test_no_data(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(FileNotFoundError):
                verify(temp)

    def test_broken_receipts(self):
        edits = [
            lambda rows: rows.pop(),
            lambda rows: rows[-1].update(observed_complete=False),
            lambda rows: rows[2].update(file="../outside.bmp"),
            lambda rows: rows[2].update(file="step-65537-frame-100-eye1.bmp"),
            lambda rows: rows[2].update(file="step-65537-frame-999-eye0.bmp", app_frame=999),
            lambda rows: rows.pop(4),
            lambda rows: rows[2].update(sha256="0" * 64),
            lambda rows: rows[2].update(app_frame=999),
            lambda rows: rows[2].update(polled_sequence=1),
            lambda rows: rows[2].update(pose_sequence=1),
            lambda rows: rows[2].update(pose_physical=0),
            lambda rows: rows[2].update(trigger=0.8),
            lambda rows: rows[2].update(owner=999),
            lambda rows: rows[3].update(model=[float("nan")] * 96),
            lambda rows: rows[3].update(indices=[-1] * 6),
            lambda rows: rows[2].update(pose_app_frame=999),
            lambda rows: rows[2].update(sequence=999),
        ]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            original = fixture(root)
            for edit in edits:
                rows = copy.deepcopy(original)
                edit(rows)
                save(root, rows)
                with self.assertRaises((Invalid, ValueError), msg=str(edit)):
                    verify(root)

    def test_swap_request_is_not_acceptance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            rows = fixture(root, "weapon-swap")
            for row in rows:
                if row.get("event") == "capture":
                    row["weapon"] = 10
            save(root, rows)
            with self.assertRaises(Invalid):
                verify(root)

    def test_tracking_rearm_is_observed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            rows = fixture(root)
            for row in rows:
                if row.get("event") == "capture" and row.get("step") == "tracked-trigger-held":
                    row["authority"] = 1
            save(root, rows)
            with self.assertRaises(Invalid):
                verify(root)

    def test_comparison_not_visual_verdict(self):
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            fixture(Path(left), "arm-sweep")
            fixture(Path(right), "arm-sweep")
            report = compare(left, right)
            self.assertEqual(report["visual_verdict"], "not_automated")
            self.assertTrue(all(r["changed_channel_fraction"] == 0 for r in report["comparison"]))


if __name__ == "__main__":
    unittest.main()
