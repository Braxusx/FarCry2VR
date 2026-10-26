"""Read-only verification of a debug scenario's receipts, not visual acceptance.

Exit 0: complete, correlated evidence (visual review still required).
Exit 1: invalid/failed evidence. Exit 2: no usable run. Never starts a game.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct

EXPECTED = {
    "weapon-swap": [(0, "baseline"), (2, "native-swap"), (3, "settled")],
    "tracking-loss": [(0, "baseline"), (1, "right-tracking-lost"), (2, "tracked-trigger-held"), (3, "neutral-rearm")],
    "arm-sweep": [(0, "baseline"), (1, "wrist-positive-200"), (2, "wrist-negative-200"), (3, "reach-out"), (4, "reach-back"), (5, "neutral")],
}


class Invalid(ValueError):
    pass


def require(value, why):
    if not value:
        raise Invalid(why)


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def integer(value):
    return type(value) is int and value > 0


def bitmap(path):
    require(path.is_file() and path.stat().st_size <= 54 + 4096 * 4096 * 3, "missing/excessive bitmap")
    data = path.read_bytes()
    require(len(data) >= 54 and data[:2] == b"BM", "bad bitmap header")
    size, offset, dib = struct.unpack_from("<I4xII", data, 2)
    width, height, planes, bits, compression = struct.unpack_from("<iiHHI", data, 18)
    require(offset == 54 and dib == 40 and planes == 1 and bits == 24 and compression == 0, "unsupported bitmap layout")
    require(0 < width <= 4096 and 0 < height <= 4096, "invalid bitmap size")
    stride = (width * 3 + 3) & ~3
    require(size == len(data) == 54 + stride * height, "truncated/extra bitmap bytes")
    return data, width, height


def verify(directory):
    directory = Path(directory).resolve()
    trace = directory / "trace.jsonl"
    if not trace.is_file() or not trace.stat().st_size:
        raise FileNotFoundError("NO_DATA: no scenario trace")
    require(trace.stat().st_size <= 64 * 1024 * 1024, "excessive trace")
    def bad_constant(value):
        raise Invalid("nonfinite JSON: " + value)
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result
    rows = [json.loads(line, parse_constant=bad_constant, object_pairs_hook=unique) for line in trace.read_text(encoding="utf-8").splitlines() if line.strip()]
    require(rows and all(type(r) is dict for r in rows), "trace must contain JSON objects")
    begin, end = rows[0], rows[-1]
    require(begin.get("event") == "begin" and begin.get("version") == "scenario-runner-1", "missing/wrong run header")
    case = begin.get("case")
    require(case in EXPECTED and begin.get("environment") in ("harness", "in_game"), "unknown case/environment")
    require(begin.get("synthetic_input") is True and begin.get("headset_accepted") is False, "missing synthetic/acceptance label")
    require(sum(r.get("event") == "begin" for r in rows) == 1 and sum(r.get("event") == "end" for r in rows) == 1, "mixed or incomplete runs")
    require(end.get("event") == "end" and end.get("case") == case and end.get("observed_complete") is True
            and end.get("reason") == "complete-observed" and end.get("headset_accepted") is False, "run did not complete observations")
    inputs = {r.get("sequence"): r for r in rows if r.get("event") == "input"}
    palette = {(r.get("ticket"), r.get("eye"), r.get("app_frame")): r for r in rows if r.get("event") == "palette"}
    groups, seen_files = {}, set()
    for r in rows:
        if r.get("event") != "capture":
            continue
        require(type(r.get("eye")) is int and r.get("eye") in (0, 1) and all(integer(r.get(k)) for k in ("ticket", "app_frame", "sequence", "owner", "weapon", "first_sequence")), "invalid capture identity")
        require(r["sequence"] >= r["first_sequence"] and r.get("polled_sequence", 0) >= r["first_sequence"]
                and r.get("polled_sequence", 0) <= r["sequence"] and r.get("polled_owner") == r["owner"], "missing matching XInput observation")
        source = inputs.get(r["sequence"])
        require(source and source.get("step") == r.get("step") and source.get("ticket") == r["ticket"], "capture has no matching input publication")
        require(number(r.get("trigger")) and r["trigger"] == 0, "unexpected emitted fire during scenario capture")
        filename = r.get("file", "")
        require(filename == f"step-{r['ticket']}-frame-{r['app_frame']}-eye{r['eye']}.bmp", "unsafe/mismatched capture filename")
        require(filename not in seen_files, "reused image file")
        seen_files.add(filename)
        path = directory / filename
        require(path.resolve().parent == directory, "capture leaves run folder")
        data, width, height = bitmap(path)
        require(hashlib.sha256(data).hexdigest() == r.get("sha256"), "capture hash mismatch")
        p = palette.get((r["ticket"], r["eye"], r["app_frame"]))
        require(p and p.get("sequence") == r.get("pose_sequence") and len(p.get("indices", [])) == 6
                and all(type(v) is int and 0 <= v < 512 for v in p["indices"])
                and len(p.get("model", [])) == 96 and all(number(v) for v in p["model"])
                and len(p.get("world", [])) == 16 and all(number(v) for v in p["world"]), "missing/invalid bounded palette copy")
        needs_pose = r["step"] not in ("right-tracking-lost", "tracked-trigger-held")
        if needs_pose:
            require(r["first_sequence"] <= r.get("pose_sequence", 0) <= r["sequence"] and 0 < r.get("pose_app_frame", 0) <= r["app_frame"] and r.get("pose_owner") == r["owner"] and r.get("pose_physical") == 1, "no current physical palette observation")
        key = r["ticket"], r["app_frame"], r["sequence"], r["owner"]
        require(r["eye"] not in groups.setdefault(key, {}), "duplicate eye receipt")
        groups[key][r["eye"]] = dict(r, width=width, height=height)
    complete = {key: eyes for key, eyes in groups.items() if set(eyes) == {0, 1}}
    selected = []
    for index, name in EXPECTED[case]:
        matches = [(key, eyes) for key, eyes in complete.items() if all(r["step"] == name for r in eyes.values()) and (key[0] & 0xffff) == index + 1]
        require(matches, "missing same-frame stereo pair for " + name)
        _, eyes = max(matches, key=lambda item: item[0][1])
        require(eyes[0]["width"] == eyes[1]["width"] and eyes[0]["height"] == eyes[1]["height"], "eye dimension mismatch")
        selected.append(eyes)
    baseline = selected[0][0]
    if case == "weapon-swap":
        changed = selected[1][0]
        require(changed["weapon"] != baseline["weapon"] and changed["owner"] != baseline["owner"], "request did not produce an observed weapon/owner change")
    if case == "tracking-loss":
        lost, held, recovered = (eyes[0] for eyes in selected[1:])
        require(lost.get("tracked") == 0 and lost.get("authority") == 0 and lost.get("hand_epoch") != baseline.get("hand_epoch"), "tracking loss was not observed")
        require(held.get("tracked") == 1 and held.get("authority") == 0, "held recovery incorrectly rearmed")
        require(recovered.get("tracked") == 1 and recovered.get("authority") == 1, "neutral recovery did not rearm")
    return {"case": case, "environment": begin["environment"], "observed_complete": True,
            "stereo_pairs": len(selected), "partial_pairs": len(groups) - len(complete),
            "visual_review": "required", "headset_accepted": False,
            "captures": [{"step": eyes[0]["step"], "left": eyes[0]["file"], "right": eyes[1]["file"],
                          "pose_frame_gap": eyes[0]["app_frame"] - eyes[0]["pose_app_frame"]} for eyes in selected]}


def compare(first, second):
    a, b = verify(first), verify(second)
    require(a["case"] == b["case"], "cannot compare different cases")
    result = []
    for x, y in zip(a["captures"], b["captures"]):
        for eye in ("left", "right"):
            p, w, h = bitmap(Path(first) / x[eye])
            q, w2, h2 = bitmap(Path(second) / y[eye])
            require((w, h) == (w2, h2), "cannot pixel-compare different dimensions")
            stride = (w * 3 + 3) & ~3
            changed = sum(p[54 + row * stride + col] != q[54 + row * stride + col] for row in range(h) for col in range(w * 3))
            result.append({"step": x["step"], "eye": eye, "changed_channel_fraction": changed / (w * h * 3)})
    return {"case": a["case"], "comparison": result, "visual_verdict": "not_automated", "headset_accepted": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--compare", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(compare(args.directory, args.compare) if args.compare else verify(args.directory), indent=2))
        return 0
    except FileNotFoundError as exc:
        print(str(exc))
        return 2
    except (Invalid, ValueError, OSError, KeyError, TypeError, struct.error) as exc:
        print("INVALID: " + str(exc))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
