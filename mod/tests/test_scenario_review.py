"""File/CPU-only controls for review tooling. Images are synthetic, not game evidence."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from test_scenario_report import event, fixture, save
from review_scenario_run import bundle, check_bundle, check_review, comparison_bundle, png, record, thumbnail
from verify_scenario_run import Invalid, digest, verify


def decode_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n": raise ValueError("signature")
    offset, chunks, pixels = 8, [], b""
    while offset < len(data):
        length, = struct.unpack_from(">I", data, offset)
        kind, body = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + length]
        checksum, = struct.unpack_from(">I", data, offset + 8 + length)
        if zlib.crc32(kind + body) != checksum: raise ValueError("CRC")
        chunks.append(kind)
        if kind == b"IHDR": width, height = struct.unpack_from(">II", body)
        if kind == b"IDAT": pixels += body
        offset += length + 12
    if chunks[0] != b"IHDR" or chunks[-1] != b"IEND" or offset != len(data): raise ValueError("layout")
    pixels = zlib.decompress(pixels)
    if len(pixels) != height * (1 + width * 3): raise ValueError("decoded dimensions")
    if any(pixels[y * (1 + width * 3)] for y in range(height)): raise ValueError("filter")
    return width, height, pixels


class Review(unittest.TestCase):
    def test_cli_only_reads_and_prepares_local_artifacts(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); run = root / "run"; run.mkdir(); fixture(run)
            tools = Path(__file__).parent
            commands = [("verify_scenario_run.py", str(run)),
                        ("review_scenario_run.py", "bundle", str(run), "--out", str(root / "bundle")),
                        ("review_scenario_run.py", "check-bundle", str(root / "bundle"))]
            for script, *args in commands:
                result = subprocess.run([sys.executable, "-B", str(tools / script), *args], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(json.loads(result.stdout)["headset_accepted"])
    def test_stereo_bundle_and_copied_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); run = root / "source"; run.mkdir(); fixture(run, "arm-sweep")
            out = root / "review"; result = bundle(run, out)
            self.assertEqual(result["samples"], 22)
            self.assertEqual(result["environment"], "harness")
            self.assertFalse(result["headset_accepted"])
            self.assertEqual(verify(run)["report_sha256"], verify(out / "run")["report_sha256"])
            self.assertEqual(len(list(out.glob("pair-*.png"))), 22)
            self.assertEqual(decode_png(out / "pair-000.png")[:2], (6, 2))
            self.assertGreater(check_bundle(out)["bundle_files_verified"], 44)
            page = (out / "REPORT.html").read_text()
            self.assertIn("not continuous video", page)
            self.assertIn("const frames=", page)
            self.assertIn('"seconds": 0.52', page)  # actual fixture clock gap, not nominal FPS
            self.assertIn("Left eye | Right eye", page)
            with self.assertRaises(FileExistsError): bundle(run, out)

    def test_comparison_copies_both_runs_and_differences(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); a = root / "a"; b = root / "b"; a.mkdir(); b.mkdir()
            fixture(a); fixture(b); out = root / "comparison"
            report = comparison_bundle(a, b, out)
            self.assertEqual(report["visual_verdict"], "not_automated")
            self.assertEqual(len(list(out.glob("comparison-*.png"))), 20)
            self.assertEqual(decode_png(out / "comparison-000-left.png")[:2], (9, 2))
            self.assertTrue((out / "baseline" / "trace.jsonl").is_file())
            self.assertTrue((out / "candidate" / "config.ini").is_file())
            check_bundle(out)

    def test_invalid_run_creates_no_bundle(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root); rows.pop(); save(root, rows)
            with self.assertRaises(Invalid): bundle(root, root / "out")
            self.assertFalse((root / "out").exists())

    def test_incompatible_comparison_creates_no_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); a = root / "a"; b = root / "b"; a.mkdir(); b.mkdir()
            fixture(a); rows = fixture(b); rows[0]["environment"] = "in_game"; save(b, rows)
            with self.assertRaises(Invalid): comparison_bundle(a, b, root / "out")
            self.assertFalse((root / "out").exists())

    def test_changed_bundle_image_detected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root); out = root / "out"; bundle(root, out)
            (out / "pair-000.png").write_bytes(b"corrupted")
            with self.assertRaisesRegex(Invalid, "changed/outside bundle file"): check_bundle(out)

    def test_rehashed_manifest_cannot_hide_inconsistent_report(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root); out = root / "out"; bundle(root, out)
            report = json.loads((out / "report.json").read_text()); report["environment"] = "in_game"
            data = json.dumps(report).encode("utf-8"); (out / "report.json").write_bytes(data)
            manifest = json.loads((out / "bundle.json").read_text()); manifest["files"]["report.json"] = digest(data)
            (out / "bundle.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(Invalid, "report does not match"): check_bundle(out)

    def test_explicit_review_is_separate_and_exclusive(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root); original = (root / "trace.jsonl").read_bytes(); path = root / "visual-review.json"
            value = record(root, path, "Synthetic reviewer", "PASS", "Synthetic fixture pixels only; not an actual game review.")
            self.assertEqual(value["environment"], "harness")
            self.assertFalse(value["headset_accepted"])
            self.assertEqual(check_review(root, path)["verdict"], "PASS")
            self.assertEqual(original, (root / "trace.jsonl").read_bytes())
            self.assertEqual(verify(root)["visual_review"], "required")
            with self.assertRaises(FileExistsError): record(root, path, "Another", "FAIL", "Do not overwrite an existing review.")

    def test_review_needs_attribution(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root)
            for reviewer, notes in (("", "long enough notes"), ("Reviewer", "ok")):
                with self.subTest(reviewer=reviewer), self.assertRaises(Invalid): record(root, root / "review.json", reviewer, "PASS", notes)

    def test_failed_run_cannot_get_visual_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root); rows[-1]["observed_complete"] = False; save(root, rows)
            with self.assertRaises(Invalid): record(root, root / "review.json", "Reviewer", "PASS", "This must not certify a failed run.")
            self.assertFalse((root / "review.json").exists())

    def test_review_invalidated_by_other_valid_receipt(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root); path = root / "visual.json"
            record(root, path, "Reviewer", "FAIL", "Synthetic review before evidence was changed.")
            event(rows, "anchor")["position"][0] = .001; save(root, rows)
            verify(root)  # valid receipt, but no longer the receipt that was reviewed
            with self.assertRaisesRegex(Invalid, "no longer matches"): check_review(root, path)

    def test_review_cannot_claim_headset_acceptance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); fixture(root); path = root / "visual.json"
            value = record(root, path, "Reviewer", "PASS", "Synthetic image review only, not a headset test.")
            value["headset_accepted"] = True; path.write_text(json.dumps(value))
            with self.assertRaises(Invalid): check_review(root, path)

    def test_bmp_orientation_and_png_channels(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); rows = fixture(root); path = root / event(rows, "capture")["file"]
            data = bytearray(path.read_bytes()); data[54:57] = b"\x00\x00\xff"; data[66:69] = b"\xff\x00\x00"; path.write_bytes(data)
            w, h, rgb = thumbnail(path)
            self.assertEqual(rgb[:3], b"\x00\x00\xff")  # top source row is blue, BMP storage is bottom-up
            self.assertEqual(rgb[w * 3:w * 3 + 3], b"\xff\x00\x00")
            preview = root / "preview.png"; png(preview, w, h, rgb)
            width, height, raw = decode_png(preview)
            self.assertEqual((width, height), (3, 2)); self.assertEqual(raw[1:4], rgb[:3])


if __name__ == "__main__":
    unittest.main()
