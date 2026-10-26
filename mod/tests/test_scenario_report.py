"""Synthetic file fixtures only. Never game, GPU or headset evidence."""
import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from verify_scenario_run import EXPECTED, Invalid, VERSION, compare, digest, samples, verify


def identity(label):
    data = label.encode("ascii")
    return {"present": True, "size": len(data), "sha256": digest(data)}


def fixture(directory, case="tracking-loss", ini="[VR]\nArmRigFix=0\nWorldScale=1\n"):
    directory = Path(directory)
    config = ini.encode("utf-8")
    (directory / "config.ini").write_bytes(config)
    provenance = {"target": {"exe": identity("synthetic-exe"), "engine": identity("synthetic-engine"), "profile": "harness-only"},
                  "mod": identity("synthetic-mod"), "config": {"present": True, "size": len(config), "sha256": digest(config)},
                  "settings": {"ArmIK": 1, "ArmRigFix": 0, "ArmWristTwistLimit": 90}, "runtime": {"name": "synthetic-not-xr", "version": 1},
                  "fixture": {"id": "head-relative-v1", "scene": "synthetic-test", "save": identity("synthetic-save"), "graphics_profile": identity("synthetic-profile"), "declared_not_loaded_proof": True},
                  "capture": {"stage": "game-eye-pre-dlss", "clock": "xr-predicted-ns", "budget_bytes": 1024 ** 3, "sampling": "bounded-motion-v1"}}
    rows = [{"event": "begin", "version": VERSION, "case": case, "environment": "harness", "synthetic_input": True, "headset_accepted": False, "provenance": provenance},
            {"event": "anchor", "position": [0, 1.7, 0], "orientation": [0, 0, 0, 1]}]
    seq, now = 100, 1000
    for index, name in EXPECTED[case]:
        count = samples(case, index)
        dwell = 4000 if name == "wrist-negative-200" else 2000 if case == "arm-sweep" and 1 <= index <= 4 else 500
        ticket = 65536 | (index + 1)
        first = seq - 2
        for sample in range(count):
            progress = sample / (count - 1) if count > 1 else 1
            milliseconds = now + round(progress * dwell)
            cap_id = (ticket << 8) | (sample + 1)
            context = {"space": 1, "world_scale": 1, "height_offset": 0, "render_scale": 1, "steady_camera": 1, "fov_mode": 1, "cull_mode": 1,
                       "stereo": True, "afw": False, "dlss": False, "overscan": [.15, .25], "reference": [0, 1.7, 0, 1, 0, 0, 0, 0, -1],
                       "fov": [-.8, .8, .8, -.8] * 2, "eyes": [-.032, 1.7, 0, 0, 0, 0, 1, .032, 1.7, 0, 0, 0, 0, 1]}
            common = {"ticket": ticket, "step": name, "sequence": seq, "generation": 1, "hand_epoch": 1, "first_sequence": first,
                      "tracked": 1, "authority": 1, "capture_id": cap_id, "sample": sample, "progress": progress,
                      "display_time_ns": 1000000000 + milliseconds * 1000000, "received_ms": milliseconds}
            if name == "right-tracking-lost": common.update(tracked=0, authority=0, hand_epoch=2)
            if name == "tracked-trigger-held": common.update(authority=0, hand_epoch=2)
            if name == "neutral-rearm": common["hand_epoch"] = 2
            rows.append(dict(common, event="input"))
            rows.append(dict(common, event="timing"))
            for eye in (0, 1):
                bmp = bytearray(78)
                bmp[:2] = b"BM"
                struct.pack_into("<I", bmp, 2, len(bmp))
                struct.pack_into("<IIiiHH", bmp, 10, 54, 40, 3, 2, 1, 24)
                bmp[54:] = bytes([sample * 40, 70 + eye * 40, 150] * 3 + [0] * 3) * 2
                file = f"capture-{cap_id}-frame-{seq}-eye{eye}.bmp"
                (directory / file).write_bytes(bmp)
                r = dict(common, event="capture", file=file, sha256=digest(bmp), eye=eye, app_frame=seq,
                         owner=1, weapon=10, pose_sequence=seq, pose_owner=1, pose_app_frame=seq, pose_physical=1,
                         pose_generation=1, pose_hand_epoch=common["hand_epoch"], named_rig=0, rig=12345,
                         polled_sequence=seq, polled_owner=1, trigger=0, capture_begin_ms=milliseconds + 2, capture_end_ms=milliseconds + 5,
                         context_stable=True, source_format=28, context=copy.deepcopy(context))
                if case == "weapon-swap" and index: r.update(owner=2, weapon=20, pose_owner=2, polled_owner=2)
                if name in ("right-tracking-lost", "tracked-trigger-held"): r["pose_physical"] = 0
                rows.append(r)
                rows.append({"event": "palette", "capture_id": cap_id, "eye": eye, "app_frame": seq, "sequence": seq,
                             "indices": list(range(6)), "model": [1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1.] * 6,
                             "world": [1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1.]})
            seq += 1
        now += dwell + 20
    rows.append({"event": "end", "case": case, "observed_complete": True, "provenance_stable": True, "reason": "complete-observed", "headset_accepted": False})
    save(directory, rows)
    return rows


def save(directory, rows):
    (Path(directory) / "trace.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")


def event(rows, name):
    return next(r for r in rows if r["event"] == name)


class Reports(unittest.TestCase):
    def test_good_harness_controls(self):
        for case in EXPECTED:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temp:
                root = Path(temp); fixture(root, case); report = verify(root)
                self.assertEqual(report["environment"], "harness")
                self.assertFalse(report["headset_accepted"])
                self.assertEqual(report["visual_review"], "required")
                self.assertEqual(report["stereo_pairs"], sum(samples(case, i) for i, _ in EXPECTED[case]))

    def test_no_data(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaises(FileNotFoundError): verify(temp)

    def test_broken_receipts(self):
        edits = {
            "missing end": lambda r: r.pop(),
            "failed end": lambda r: r[-1].update(observed_complete=False),
            "changed provenance": lambda r: r[-1].update(provenance_stable=False),
            "legacy header": lambda r: r[0].update(version="scenario-runner-1"),
            "missing target hash": lambda r: r[0]["provenance"]["target"]["engine"].update(sha256="unknown"),
            "missing anchor": lambda r: r.remove(event(r, "anchor")),
            "path escape": lambda r: event(r, "capture").update(file="../outside.bmp"),
            "wrong hash": lambda r: event(r, "capture").update(sha256="0" * 64),
            "old poll": lambda r: event(r, "capture").update(polled_sequence=1),
            "old pose": lambda r: event(r, "capture").update(pose_sequence=1),
            "nonphysical": lambda r: event(r, "capture").update(pose_physical=0),
            "fire": lambda r: event(r, "capture").update(trigger=.8),
            "wrong owner": lambda r: event(r, "capture").update(owner=99),
            "nan palette": lambda r: event(r, "palette").update(model=[float("nan")] * 96),
            "negative index": lambda r: event(r, "palette").update(indices=[-1] * 6),
            "duplicate helper": lambda r: event(r, "palette").update(indices=[1] * 6),
            "future pose frame": lambda r: event(r, "capture").update(pose_app_frame=99999),
            "wrong input": lambda r: event(r, "capture").update(sequence=99999),
            "expired readback": lambda r: event(r, "capture").update(capture_end_ms=99999),
            "context changed": lambda r: event(r, "capture").update(context_stable=False),
            "wrong source clock": lambda r: event(r, "capture").update(display_time_ns=1),
            "wrong generation": lambda r: event(r, "capture").update(pose_generation=99),
            "wrong hand epoch": lambda r: event(r, "capture").update(pose_hand_epoch=99),
            "unsupported profile": lambda r: event(r, "capture")["context"].update(afw=True),
            "missing context": lambda r: event(r, "capture").pop("context"),
            "missing source format": lambda r: event(r, "capture").pop("source_format"),
            "duplicate input": lambda r: r.insert(-1, copy.deepcopy(event(r, "input"))),
            "duplicate palette": lambda r: r.insert(-1, copy.deepcopy(event(r, "palette"))),
        }
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); original = fixture(root)
            for name, edit in edits.items():
                with self.subTest(name=name):
                    rows = copy.deepcopy(original); edit(rows); save(root, rows)
                    with self.assertRaises((Invalid, ValueError)): verify(root)

    def test_mixed_applied_pose_sequences_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root, "arm-sweep")
            earlier_input = dict(event(rows, "input"), sequence=99)
            earlier_clock = dict(event(rows, "timing"), sequence=99)
            rows[2:2] = [earlier_input, earlier_clock]
            event(rows, "capture").update(pose_sequence=99, pose_app_frame=99)
            event(rows, "palette")["sequence"] = 99
            save(root, rows)
            with self.assertRaisesRegex(Invalid, "stereo applied-pose"): verify(root)

    def test_missing_intermediate_samples(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root, "arm-sweep")
            rows = [r for r in rows if not (r["event"] in ("capture", "palette") and (r["capture_id"] & 255) == 3)]
            save(root, rows)
            with self.assertRaisesRegex(Invalid, "missing stereo motion sample"): verify(root)

    def test_endpoint_duplicates_are_not_motion(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root, "arm-sweep")
            for row in rows:
                if row.get("step") == "wrist-positive-200": row["progress"] = 1
            save(root, rows)
            with self.assertRaisesRegex(Invalid, "sampling window"): verify(root)

    def test_swap_request_is_not_acceptance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root, "weapon-swap")
            for row in rows:
                if row["event"] == "capture": row["weapon"] = 10
            save(root, rows)
            with self.assertRaises(Invalid): verify(root)

    def test_tracking_rearm_is_observed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root)
            for row in rows:
                if row.get("step") == "tracked-trigger-held" and row["event"] in ("input", "capture"): row["authority"] = 1
            save(root, rows)
            with self.assertRaisesRegex(Invalid, "held recovery"): verify(root)

    def test_comparison_not_visual_verdict(self):
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            fixture(left, "arm-sweep"); fixture(right, "arm-sweep"); report = compare(left, right)
            self.assertEqual(report["visual_verdict"], "not_automated")
            self.assertEqual(report["environment"], "harness")
            self.assertTrue(all(r["changed_channel_fraction"] == 0 for r in report["comparison"]))

    def test_incompatible_baselines(self):
        edits = {
            "environment": lambda r: r[0].update(environment="in_game"),
            "anchor": lambda r: event(r, "anchor")["position"].__setitem__(0, 10),
            "save": lambda r: r[0]["provenance"]["fixture"].update(save=identity("other-save")),
            "profile": lambda r: r[0]["provenance"]["fixture"].update(graphics_profile=identity("other-profile")),
            "target": lambda r: r[0]["provenance"]["target"].update(engine=identity("other-engine")),
            "runtime": lambda r: r[0]["provenance"]["runtime"].update(version=2),
            "mod": lambda r: r[0]["provenance"].update(mod=identity("other-mod")),
        }
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            fixture(left, "arm-sweep"); original = fixture(right, "arm-sweep")
            for name, edit in edits.items():
                with self.subTest(name=name):
                    rows = copy.deepcopy(original); edit(rows); save(right, rows)
                    with self.assertRaises(Invalid): compare(left, right)

    def test_explicit_arm_ab_allowances(self):
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            fixture(left, "arm-sweep")
            rows = fixture(right, "arm-sweep", "[VR]\nArmRigFix=1\nWorldScale=1\n")
            rows[0]["provenance"]["settings"]["ArmRigFix"] = 1
            rows[0]["provenance"]["mod"] = identity("changed-mod")
            for row in rows:
                if row["event"] == "capture": row["named_rig"] = 1
            save(right, rows)
            with self.assertRaises(Invalid): compare(left, right, ["mod"])
            report = compare(left, right, ["mod", "ArmRigFix"])
            self.assertIn("config.vr.armrigfix", report["observed_changes"])
            self.assertIn("settings.ArmRigFix", report["observed_changes"])
            with self.assertRaises(Invalid): compare(left, right, ["target"])

    def test_ini_changes_cannot_hide_behind_mod_allowance(self):
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            fixture(left); fixture(right, ini="[VR]\nArmRigFix=0\nWorldScale=2\n")
            with self.assertRaisesRegex(Invalid, "undeclared INI change"): compare(left, right, ["mod", "ArmRigFix"])

    def test_changed_configuration_artifact(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root); (root / "config.ini").write_text("[VR]\nArmRigFix=1\n")
            with self.assertRaisesRegex(Invalid, "configuration snapshot changed"): verify(root)

    def test_valid_but_incompatible_capture_contexts(self):
        changes = {
            "scale": lambda r: r["context"].update(world_scale=2),
            "fov": lambda r: r["context"]["fov"].__setitem__(0, -.7),
            "ipd": lambda r: r["context"]["eyes"].__setitem__(7, .034),
            "reference": lambda r: r["context"]["reference"].__setitem__(0, 1),
            "eye pose": lambda r: r["context"]["eyes"].__setitem__(1, 1.72),
            "topology": lambda r: r.update(rig=98765),
            "source format": lambda r: r.update(source_format=10),
        }
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            fixture(left, "arm-sweep"); original = fixture(right, "arm-sweep")
            for name, change in changes.items():
                with self.subTest(name=name):
                    rows = copy.deepcopy(original)
                    for row in rows:
                        if row["event"] == "capture": change(row)
                    save(right, rows); verify(right)
                    with self.assertRaises(Invalid): compare(left, right)

    def test_quaternion_sign_is_not_a_fixture_change(self):
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            fixture(left); rows = fixture(right)
            event(rows, "anchor")["orientation"] = [0, 0, 0, -1]
            for row in rows:
                if row["event"] == "capture":
                    row["context"]["eyes"][6] = row["context"]["eyes"][13] = -1
            save(right, rows); compare(left, right)

    def test_duplicate_json_keys(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root)
            text = (root / "trace.jsonl").read_text().replace('"version":', '"version":"duplicate", "version":', 1)
            (root / "trace.jsonl").write_text(text)
            with self.assertRaisesRegex(Invalid, "duplicate JSON key"): verify(root)


if __name__ == "__main__":
    unittest.main()
