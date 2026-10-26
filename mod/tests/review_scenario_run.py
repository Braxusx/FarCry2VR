"""Offline, self-contained sampled-eye review bundles. Never launches anything.

HTML playback uses recorded XR timestamp intervals; it is not continuous video,
measured performance, final compositor output or headset/comfort acceptance.
"""
import argparse
import datetime
import html
import json
from pathlib import Path
import struct
import zlib
from verify_scenario_run import ALLOW_CHANGES, Invalid, bitmap, canonical, compare, digest, require, strict_json, verify


def write_json(path, value):
    with path.open("x", encoding="utf-8") as file:
        json.dump(value, file, indent=2, allow_nan=False)
        file.write("\n")


def artifact(root, name, expected):
    path = root / name
    require(path.resolve().parent == root.resolve(), "artifact outside run folder")
    data = path.read_bytes()
    require(digest(data) == expected, "artifact changed during review preparation: " + name)
    return data


def thumbnail(path, width=320):
    data, w, h = bitmap(path)
    out_w = min(width, w)
    out_h = max(1, round(h * out_w / w))
    stride = (w * 3 + 3) & ~3
    pixels = bytearray(out_w * out_h * 3)
    for y in range(out_h):
        source_y = h - 1 - min(h - 1, y * h // out_h)
        for x in range(out_w):
            offset = 54 + source_y * stride + min(w - 1, x * w // out_w) * 3
            start = (y * out_w + x) * 3
            pixels[start:start + 3] = data[offset:offset + 3][::-1]
    return out_w, out_h, bytes(pixels)


def png(path, w, h, rgb):
    require(len(rgb) == w * h * 3 and 0 < w <= 1920 and 0 < h <= 4096, "invalid preview bounds")
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value))
    scan = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(scan)) + chunk(b"IEND", b"")
    with path.open("xb") as file:
        file.write(data)


def contact(path, images):
    w, h = images[0][:2]
    require(all(im[:2] == (w, h) for im in images), "contact-sheet dimensions mismatch")
    rgb = b"".join(b"".join(im[2][y * w * 3:(y + 1) * w * 3] for im in images) for y in range(h))
    png(path, w * len(images), h, rgb)


def bundle_reports(out):
    if (out / "run").is_dir():
        report = verify(out / "run")
        require(strict_json((out / "report.json").read_text(encoding="utf-8")) == report, "bundle report does not match copied evidence")
        return [report["report_sha256"]]
    require((out / "baseline").is_dir() and (out / "candidate").is_dir(), "bundle has no complete copied evidence")
    saved = strict_json((out / "comparison.json").read_text(encoding="utf-8"))
    result = compare(out / "baseline", out / "candidate", saved.get("allowed_changes", []))
    require(result == saved, "bundle comparison does not match copied evidence")
    return [result["baseline_report_sha256"], result["candidate_report_sha256"]]


def seal(out, report_hashes):
    require(bundle_reports(out) == report_hashes, "evidence changed while preparing bundle")
    files = {}
    for file in sorted(out.rglob("*")):
        if file.is_file():
            require(not file.is_symlink() and file.resolve().is_relative_to(out.resolve()), "outside bundle artifact")
            files[file.relative_to(out).as_posix()] = digest(file.read_bytes())
    write_json(out / "bundle.json", {"schema": 1, "kind": "sampled-review-not-acceptance", "report_hashes": report_hashes, "files": files})


def check_bundle(folder):
    folder = Path(folder).resolve()
    manifest = strict_json((folder / "bundle.json").read_text(encoding="utf-8"))
    require(manifest.get("schema") == 1 and manifest.get("kind") == "sampled-review-not-acceptance", "unknown review bundle")
    require(type(manifest.get("files")) is dict and 0 < len(manifest["files"]) <= 1024, "invalid bundle manifest")
    for name, expected in manifest["files"].items():
        path = (folder / name).resolve()
        require(path.is_relative_to(folder) and path != folder / "bundle.json" and path.is_file() and digest(path.read_bytes()) == expected, "missing/changed/outside bundle file: " + name)
    require(bundle_reports(folder) == manifest.get("report_hashes"), "bundle receipt identity mismatch")
    return {"bundle_files_verified": len(manifest["files"]), "visual_review": "not_inferred", "headset_accepted": False}


def copied_run(root, out, report):
    out.mkdir()
    for name, sha in report["artifact_hashes"].items():
        with (out / name).open("xb") as file:
            file.write(artifact(root, name, sha))
    require(verify(out)["report_sha256"] == report["report_sha256"], "copied receipt changed identity")


STYLE = "body{background:#14191f;color:#eee;font:16px system-ui;margin:24px;max-width:1500px}a{color:#9ed8ff}img{max-width:100%;height:auto}section{border-top:1px solid #657;margin-top:20px;padding-top:10px}button,input{margin:8px}pre{white-space:pre-wrap;overflow-wrap:anywhere}.warning{color:#ffd080}"


def bundle(run, out):
    root, out = Path(run).resolve(), Path(out).resolve()
    report = verify(root)  # validate before creating an output directory
    out.mkdir(exist_ok=False)
    copied_run(root, out / "run", report)
    write_json(out / "report.json", report)
    frames, sections = [], []
    first_time = report["captures"][0]["display_time_ns"]
    for index, cap in enumerate(report["captures"]):
        images = [thumbnail(out / "run" / cap[eye], 640) for eye in ("left", "right")]
        preview = f"pair-{index:03}.png"
        contact(out / preview, images)
        label = f'{cap["step"]} / sample {cap["sample"]} / progress {cap["progress"]:.3f}'
        seconds = (cap["display_time_ns"] - first_time) / 1e9
        frames.append({"image": preview, "seconds": seconds, "label": label, "xr_ns": str(cap["display_time_ns"])})
        links = " · ".join(f'<a href="run/{html.escape(cap[e], quote=True)}">original {e}</a>' for e in ("left", "right"))
        sections.append(f'<section><h2>{html.escape(label)}</h2><p>XR ns {cap["display_time_ns"]}; app frame {cap["app_frame"]}; writer source {cap["pose_sequence"]}; writer XR ns {cap["applied_display_time_ns"]}; writer progress {cap["applied_progress"]}; pose-frame gaps {cap["pose_frame_gaps"]}. {links}</p><p>Left eye | Right eye</p><img src="{preview}" alt="left and right sampled eyes"></section>')
    metadata = html.escape(json.dumps({k: report[k] for k in ("environment", "case", "mechanical", "report_sha256", "provenance", "anchor")}, indent=2))
    script = json.dumps(frames).replace("<", "\\u003c")
    page = '<!doctype html><meta charset="utf-8"><title>FC2 sampled motion review</title><style>' + STYLE + '</style><h1>FC2 sampled motion — review pending</h1>'
    page += '<p class="warning">Environment: ' + html.escape(report["environment"]) + '. ' + html.escape(report["limits"]) + '</p><pre>' + metadata + '</pre>'
    page += '<h2>Timestamped sample replay</h2><p>Left eye | Right eye. Timing is between recorded samples, not measured playback FPS. Use the slider to inspect every sample.</p><button id="play">Replay</button><button id="stop">Pause</button><input id="sample" type="range" min="0" max="' + str(len(frames) - 1) + '" value="0"><p id="label"></p><img id="frame" alt="sampled stereo pair">'
    page += '<script>const frames=' + script + ';let index=0,timer=null;const slider=document.getElementById("sample");function pause(){clearTimeout(timer);timer=null;}function show(){const f=frames[index];document.getElementById("frame").src=f.image;document.getElementById("label").textContent=f.label+" / +"+f.seconds.toFixed(3)+"s / XR ns "+f.xr_ns;slider.value=index;}function next(){show();if(index+1<frames.length){const wait=(frames[index+1].seconds-frames[index].seconds)*1000;timer=setTimeout(()=>{index++;next();},wait);}}document.getElementById("play").onclick=()=>{pause();index=0;next();};document.getElementById("stop").onclick=pause;slider.oninput=()=>{pause();index=Number(slider.value);show();};show();</script>'
    page += "".join(sections)
    with (out / "REPORT.html").open("x", encoding="utf-8") as file:
        file.write(page)
    seal(out, [report["report_sha256"]])
    return {"out": str(out), "samples": len(frames), "environment": report["environment"], "visual_review": "required", "headset_accepted": False}


def comparison_bundle(first, second, out, allow_changes=()):
    result = compare(first, second, allow_changes)
    a, b = verify(first), verify(second)
    require((a["report_sha256"], b["report_sha256"]) == (result["baseline_report_sha256"], result["candidate_report_sha256"]), "comparison source changed")
    out = Path(out).resolve(); out.mkdir(exist_ok=False)
    copied_run(Path(first).resolve(), out / "baseline", a)
    copied_run(Path(second).resolve(), out / "candidate", b)
    write_json(out / "comparison.json", result)
    sections = []
    for index, (left, right) in enumerate(zip(a["captures"], b["captures"])):
        for eye in ("left", "right"):
            before = thumbnail(out / "baseline" / left[eye]); after = thumbnail(out / "candidate" / right[eye])
            require(before[:2] == after[:2], "comparison preview dimensions mismatch")
            difference = (before[0], before[1], bytes(abs(x - y) for x, y in zip(before[2], after[2])))
            name = f"comparison-{index:03}-{eye}.png"
            contact(out / name, [before, after, difference])
            sections.append(f'<section><h2>{html.escape(left["step"])} / {left["sample"]} / {eye}</h2><p>Before | After | Absolute difference (downsampled review aid)</p><img src="{name}" alt="before, after, absolute difference"></section>')
    page = '<!doctype html><meta charset="utf-8"><title>FC2 before/after review</title><style>' + STYLE + '</style><h1>Before / after — advisory, review pending</h1><p class="warning">Pixel differences do not determine correctness or headset acceptance.</p><pre>' + html.escape(json.dumps(result, indent=2)) + '</pre>' + "".join(sections)
    with (out / "REPORT.html").open("x", encoding="utf-8") as file:
        file.write(page)
    seal(out, [a["report_sha256"], b["report_sha256"]])
    return result


def record(run, out, reviewer, verdict, notes):
    require(verdict in ("PASS", "FAIL") and 1 <= len(reviewer.strip()) <= 200 and 10 <= len(notes.strip()) <= 4096, "reviewer and substantive notes required")
    report = verify(run)  # incomplete/failed mechanical evidence cannot gain visual PASS
    value = {"schema": 1, "kind": "explicit-sampled-image-review", "reviewer": reviewer.strip(), "verdict": verdict, "notes": notes.strip(),
             "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "report_sha256": report["report_sha256"],
             "artifact_hashes": report["artifact_hashes"], "environment": report["environment"], "scope": "sampled-game-eye-images-only", "headset_accepted": False}
    write_json(Path(out), value)  # exclusive create; never rewrites a review or mechanical receipt
    return value


def check_review(run, path):
    path = Path(path)
    require(path.stat().st_size <= 128 * 1024, "excessive review record")
    value = strict_json(path.read_text(encoding="utf-8")); report = verify(run)
    require(value.get("schema") == 1 and value.get("kind") == "explicit-sampled-image-review" and value.get("verdict") in ("PASS", "FAIL")
            and value.get("scope") == "sampled-game-eye-images-only" and value.get("headset_accepted") is False, "unknown review scope/verdict")
    require(value.get("report_sha256") == report["report_sha256"] and value.get("artifact_hashes") == report["artifact_hashes"] and value.get("environment") == report["environment"], "review no longer matches current evidence")
    require(type(value.get("reviewer")) is str and value["reviewer"].strip() and type(value.get("notes")) is str and len(value["notes"].strip()) >= 10, "unattributed review")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    single = sub.add_parser("bundle"); single.add_argument("run", type=Path); single.add_argument("--out", type=Path, required=True)
    comp = sub.add_parser("compare"); comp.add_argument("baseline", type=Path); comp.add_argument("candidate", type=Path); comp.add_argument("--out", type=Path, required=True)
    comp.add_argument("--allow-change", action="append", choices=ALLOW_CHANGES, default=[])
    rev = sub.add_parser("record"); rev.add_argument("run", type=Path); rev.add_argument("--out", type=Path, required=True)
    rev.add_argument("--reviewer", required=True); rev.add_argument("--verdict", choices=("PASS", "FAIL"), required=True); rev.add_argument("--notes", required=True)
    check = sub.add_parser("check-review"); check.add_argument("run", type=Path); check.add_argument("review", type=Path)
    sealed = sub.add_parser("check-bundle"); sealed.add_argument("bundle", type=Path)
    args = parser.parse_args()
    try:
        if args.operation == "bundle": value = bundle(args.run, args.out)
        elif args.operation == "compare": value = comparison_bundle(args.baseline, args.candidate, args.out, args.allow_change)
        elif args.operation == "record": value = record(args.run, args.out, args.reviewer, args.verdict, args.notes)
        elif args.operation == "check-review": value = check_review(args.run, args.review)
        else: value = check_bundle(args.bundle)
        print(json.dumps(value, indent=2)); return 0
    except (ValueError, OSError, KeyError, TypeError, struct.error) as error:
        print("INVALID: " + str(error)); return 1


if __name__ == "__main__":
    raise SystemExit(main())
