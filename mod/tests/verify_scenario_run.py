"""Offline verification, not visual/native-consumer/headset acceptance.

Exit 0: correlated sampled evidence, review pending. 1: invalid/failed evidence.
2: no trace. Old v1 receipts stay historical; they cannot certify v2 provenance.
"""
import argparse
import configparser
import hashlib
import json
import math
from pathlib import Path
import re
import struct

VERSION = "scenario-runner-2"
EXPECTED = {
    "weapon-swap": [(0, "baseline"), (2, "native-swap"), (3, "settled")],
    "tracking-loss": [(0, "baseline"), (1, "right-tracking-lost"), (2, "tracked-trigger-held"), (3, "neutral-rearm")],
    "arm-sweep": [(0, "baseline"), (1, "wrist-positive-200"), (2, "wrist-negative-200"), (3, "reach-out"), (4, "reach-back"), (5, "neutral")],
}
ALLOW_CHANGES = ("mod", "ArmRigFix", "ArmWristTwistLimit")
MAX_BYTES = 1024 ** 3


class Invalid(ValueError):
    pass


def require(value, why):
    if not value:
        raise Invalid(why)


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def integer(value):
    return type(value) is int and value > 0


def vector(value, count):
    return type(value) is list and len(value) == count and all(number(v) for v in value)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def strict_json(text):
    def constant(value):
        raise Invalid("nonfinite JSON: " + value)
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: " + key)
            result[key] = value
        return result
    return json.loads(text, parse_constant=constant, object_pairs_hook=unique)


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


def samples(case, index):
    return 5 if case == "arm-sweep" and 1 <= index <= 4 else 3 if case == "tracking-loss" and 0 < index < 4 else 1


def file_id(value, absent=False):
    require(type(value) is dict and set(value) == {"present", "size", "sha256"}, "invalid file identity")
    require(type(value["present"]) is bool and type(value["size"]) is int and 0 <= value["size"] <= 256 * 1024 * 1024, "invalid file size/presence")
    if value["present"]:
        require(type(value["sha256"]) is str and re.fullmatch(r"[0-9a-f]{64}", value["sha256"]), "missing full SHA-256")
    else:
        require(absent and value["size"] == 0 and value["sha256"] == "", "required file absent")


def config_snapshot(directory, identity, artifacts):
    file_id(identity, absent=True)
    if not identity["present"]:
        require(not (directory / "config.ini").exists(), "undeclared configuration snapshot")
        return {}
    path = directory / "config.ini"
    require(path.resolve().parent == directory and path.is_file() and path.stat().st_size <= 2 * 1024 * 1024, "missing/excessive configuration snapshot")
    data = path.read_bytes()
    require(len(data) == identity["size"] and digest(data) == identity["sha256"], "configuration snapshot changed")
    artifacts["config.ini"] = digest(data)
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    try:
        text = data.decode(encoding)
    except UnicodeDecodeError:
        text = data.decode("cp1252")
    parser = configparser.ConfigParser(interpolation=None, strict=True, default_section="__NO_DEFAULTS__")
    parser.optionxform = str.casefold
    try:
        parser.read_string(text)
    except configparser.Error as error:
        raise Invalid("ambiguous configuration snapshot: " + str(error)) from error
    require(not parser.defaults(), "INI defaults are not a Win32 configuration contract")
    result = {}
    for section in parser.sections():
        key = section.casefold()
        require(key not in result, "case-colliding INI sections")
        result[key] = dict(parser.items(section))
    return result


def provenance(begin, directory, artifacts):
    p = begin.get("provenance")
    require(type(p) is dict and set(p) == {"target", "mod", "config", "settings", "runtime", "fixture", "capture"}, "missing/unknown provenance fields")
    t = p["target"]
    require(type(t) is dict and set(t) == {"exe", "engine", "profile"} and type(t["profile"]) is str and t["profile"], "missing target identity/profile")
    file_id(t["exe"]); file_id(t["engine"]); file_id(p["mod"])
    ini = config_snapshot(directory, p["config"], artifacts)
    s = p["settings"]
    require(type(s) is dict and set(s) == {"ArmIK", "ArmRigFix", "ArmWristTwistLimit"}, "missing effective rig settings")
    require(all(type(s[k]) is int and s[k] in (0, 1) for k in ("ArmIK", "ArmRigFix")) and number(s["ArmWristTwistLimit"]) and 0 <= s["ArmWristTwistLimit"] <= 180, "invalid effective rig settings")
    runtime = p["runtime"]
    require(type(runtime) is dict and set(runtime) == {"name", "version"} and type(runtime["name"]) is str and 0 < len(runtime["name"]) < 256 and integer(runtime["version"]), "missing runtime identity")
    f = p["fixture"]
    require(type(f) is dict and set(f) == {"id", "scene", "save", "graphics_profile", "declared_not_loaded_proof"}, "missing fixture provenance")
    require(f["id"] == "head-relative-v1" and type(f["scene"]) is str and re.fullmatch(r"[A-Za-z0-9_-]{1,80}", f["scene"]) and f["declared_not_loaded_proof"] is True, "invalid/unlabelled fixture declaration")
    file_id(f["save"]); file_id(f["graphics_profile"])
    require(f["save"]["size"] > 0 and f["graphics_profile"]["size"] > 0, "empty fixture files")
    require(p["capture"] == {"stage": "game-eye-pre-dlss", "clock": "xr-predicted-ns", "budget_bytes": MAX_BYTES, "sampling": "bounded-motion-v1"}, "unsupported capture provenance")
    return p, ini


def context(value):
    keys = {"space", "world_scale", "height_offset", "render_scale", "steady_camera", "fov_mode", "cull_mode", "stereo", "afw", "dlss", "overscan", "reference", "fov", "eyes"}
    require(type(value) is dict and set(value) == keys, "missing capture context")
    require(integer(value["space"]) and number(value["world_scale"]) and value["world_scale"] > 0 and number(value["render_scale"]) and value["render_scale"] > 0 and number(value["height_offset"]), "invalid capture scale/reference")
    require(all(type(value[k]) is int and value[k] >= 0 for k in ("steady_camera", "fov_mode", "cull_mode")), "invalid render policy")
    require(value["stereo"] is True and value["afw"] is False and value["dlss"] is False, "unsupported/reconstructed render profile")
    require(all(vector(value[k], n) for k, n in (("overscan", 2), ("reference", 9), ("fov", 8), ("eyes", 14))), "nonfinite/incomplete capture context")
    for eye in range(2):
        left, right, up, down = value["fov"][eye * 4:eye * 4 + 4]
        require(-math.pi / 2 < left < 0 < right < math.pi / 2 and -math.pi / 2 < down < 0 < up < math.pi / 2, "invalid signed eye FOV")
        q = value["eyes"][eye * 7 + 3:eye * 7 + 7]
        require(abs(sum(v * v for v in q) - 1) < .002, "nonunit eye orientation")
    right, forward = value["reference"][3:6], value["reference"][6:9]
    require(all(abs(sum(v * v for v in axis) - 1) < .002 for axis in (right, forward)) and abs(sum(x * y for x, y in zip(right, forward))) < .002, "invalid reference basis")
    return value


def indexed(rows, event, key):
    result = {}
    for row in rows:
        if row.get("event") == event:
            identity = key(row)
            require(identity not in result, "duplicate " + event + " identity")
            result[identity] = row
    return result


def verify(directory):
    directory = Path(directory).resolve()
    trace = directory / "trace.jsonl"
    if not trace.is_file() or not trace.stat().st_size:
        raise FileNotFoundError("NO_DATA: no scenario trace")
    require(trace.resolve().parent == directory and trace.stat().st_size <= 64 * 1024 * 1024, "excessive/outside trace")
    raw = trace.read_bytes()
    rows = [strict_json(line) for line in raw.decode("utf-8").splitlines() if line.strip()]
    require(0 < len(rows) <= 20000 and all(type(r) is dict for r in rows), "trace must contain bounded JSON objects")
    require(all(r.get("event") in {"begin", "anchor", "input", "timing", "capture", "palette", "end"} for r in rows), "unknown trace event")
    begin, end = rows[0], rows[-1]
    require(begin.get("event") == "begin" and begin.get("version") == VERSION, "missing/wrong run header; v1 cannot certify v2 provenance")
    case = begin.get("case")
    require(case in EXPECTED and begin.get("environment") in ("harness", "in_game"), "unknown case/environment")
    require(begin.get("synthetic_input") is True and begin.get("headset_accepted") is False, "missing synthetic/acceptance label")
    require(sum(r.get("event") == "begin" for r in rows) == 1 and sum(r.get("event") == "end" for r in rows) == 1, "mixed or incomplete runs")
    require(end.get("event") == "end" and end.get("case") == case and end.get("observed_complete") is True and end.get("provenance_stable") is True
            and end.get("reason") == "complete-observed" and end.get("headset_accepted") is False, "run did not complete stable observations")
    artifacts = {"trace.jsonl": digest(raw)}
    p, ini = provenance(begin, directory, artifacts)
    require(case != "arm-sweep" or p["settings"]["ArmIK"] == 1, "arm sweep requires ArmIK=1")
    anchors = [r for r in rows if r.get("event") == "anchor"]
    require(len(anchors) == 1 and vector(anchors[0].get("position"), 3) and vector(anchors[0].get("orientation"), 4), "missing/ambiguous anchor")
    anchor = {k: anchors[0][k] for k in ("position", "orientation")}
    require(abs(sum(x * x for x in anchor["orientation"]) - 1) < .002, "nonunit anchor")
    inputs = indexed(rows, "input", lambda r: r["sequence"])
    timing = indexed(rows, "timing", lambda r: r["sequence"])
    require(inputs.keys() == timing.keys(), "unpaired input/timing publications")
    previous_sequence = 0
    for source in inputs.values():
        require(all(integer(source.get(k)) for k in ("sequence", "generation", "hand_epoch")) and source["sequence"] > previous_sequence, "invalid/regressing input publication")
        previous_sequence = source["sequence"]
    palettes = indexed(rows, "palette", lambda r: (r["capture_id"], r["eye"], r["app_frame"]))
    groups, files, total_bytes = {}, set(), 0
    capture_rows = [r for r in rows if r.get("event") == "capture"]
    require(0 < len(capture_rows) <= 256, "missing/excessive capture records")
    for r in capture_rows:
        require(type(r.get("eye")) is int and r["eye"] in (0, 1) and all(integer(r.get(k)) for k in ("capture_id", "ticket", "app_frame", "sequence", "owner", "weapon", "first_sequence", "generation", "rig", "hand_epoch")), "invalid capture identity")
        require(type(r.get("sample")) is int and r["sample"] >= 0 and number(r.get("progress")) and 0 <= r["progress"] <= 1, "invalid motion sample")
        steps = dict((name, index) for index, name in EXPECTED[case])
        require(r.get("step") in steps and r["sample"] < samples(case, steps[r["step"]]) and (r["ticket"] >> 16) > 0
                and (r["ticket"] & 0xffff) == steps[r["step"]] + 1, "unexpected step/sample/run ticket")
        require(r["capture_id"] == ((r["ticket"] << 8) | (r["sample"] + 1)), "capture/sample ticket mismatch")
        require(r["sequence"] >= r["first_sequence"] and integer(r.get("polled_sequence")) and r["first_sequence"] <= r["polled_sequence"] <= r["sequence"] and r.get("polled_owner") == r["owner"], "missing matching XInput observation")
        source, clock = inputs.get(r["sequence"]), timing.get(r["sequence"])
        require(source and clock and source.get("step") == r.get("step") and source.get("ticket") == r["ticket"] and source.get("generation") == r["generation"]
                and source.get("hand_epoch") == r["hand_epoch"] and source.get("tracked") == r.get("tracked") and source.get("authority") == r.get("authority"), "capture has no matching input/context publication")
        require(all(clock.get(k) == r.get(k) for k in ("sample", "capture_id", "progress", "received_ms", "display_time_ns")), "capture clock/sample not published")
        require(type(r.get("display_time_ns")) is int and r["display_time_ns"] != 0 and all(integer(r.get(k)) for k in ("received_ms", "capture_begin_ms", "capture_end_ms")), "missing capture clocks")
        require(r["received_ms"] <= r["capture_begin_ms"] <= r["capture_end_ms"] <= r["received_ms"] + 100 and r.get("context_stable") is True, "capture crossed/expired its input lease")
        require(number(r.get("trigger")) and r["trigger"] == 0, "unexpected emitted fire during capture")
        require(type(r.get("source_format")) is int and r["source_format"] in (2, 10, 28, 29, 87, 91), "missing/unsupported source texture format")
        filename = r.get("file", "")
        require(filename == f"capture-{r['capture_id']}-frame-{r['app_frame']}-eye{r['eye']}.bmp" and filename not in files, "unsafe/reused/mismatched capture filename")
        files.add(filename)
        path = directory / filename
        require(path.resolve().parent == directory, "capture leaves run folder")
        data, width, height = bitmap(path)
        require(digest(data) == r.get("sha256"), "capture hash mismatch")
        artifacts[filename] = digest(data); total_bytes += len(data)
        require(total_bytes <= MAX_BYTES, "capture byte budget exceeded")
        palette = palettes.get((r["capture_id"], r["eye"], r["app_frame"]))
        require(palette and palette.get("sequence") == r.get("pose_sequence") and type(palette.get("indices")) is list and len(palette["indices"]) == 6
                and all(type(v) is int and 0 <= v < 512 for v in palette["indices"]) and len(set(palette["indices"])) == 6
                and vector(palette.get("model"), 96) and vector(palette.get("world"), 16), "missing/invalid bounded palette copy")
        require(all(integer(r.get(k)) for k in ("pose_sequence", "pose_owner", "pose_app_frame", "pose_generation", "pose_hand_epoch"))
                and r["pose_sequence"] <= r["sequence"] and r["pose_app_frame"] <= r["app_frame"], "future/invalid visual palette observation")
        require(all(type(r.get(k)) is int and r[k] in (0, 1) for k in ("pose_physical", "named_rig", "tracked", "authority")) and r["named_rig"] == p["settings"]["ArmRigFix"], "invalid pose/authority/rig policy")
        needs_pose = r["step"] not in ("right-tracking-lost", "tracked-trigger-held")
        if needs_pose:
            require(r["tracked"] == 1 and r["authority"] == 1, "capture has no current tracked/rearmed authority")
            require(r["first_sequence"] <= r.get("pose_sequence", 0) <= r["sequence"] and 0 < r.get("pose_app_frame", 0) <= r["app_frame"] and r.get("pose_owner") == r["owner"]
                    and r["pose_physical"] == 1 and r.get("pose_generation") == r["generation"] and r.get("pose_hand_epoch") == r["hand_epoch"], "no current physical palette observation")
            applied_input = inputs.get(r["pose_sequence"])
            require(applied_input and applied_input.get("ticket") == r["ticket"] and applied_input.get("step") == r["step"], "applied pose has no matching step publication")
        c = context(r.get("context"))
        key = r["capture_id"], r["app_frame"]
        require(r["eye"] not in groups.setdefault(key, {}), "duplicate eye receipt")
        groups[key][r["eye"]] = dict(r, width=width, height=height, palette=palette, context=c)
    complete = []
    same = ("ticket", "step", "sample", "progress", "sequence", "first_sequence", "generation", "owner", "weapon", "pose_sequence", "pose_owner", "pose_app_frame", "pose_generation", "pose_hand_epoch", "pose_physical", "named_rig", "rig", "tracked", "authority", "hand_epoch", "display_time_ns", "received_ms", "source_format", "context", "width", "height")
    for eyes in groups.values():
        if set(eyes) == {0, 1}:
            require(all(eyes[0].get(k) == eyes[1].get(k) for k in same), "stereo applied-pose/context mismatch")
            complete.append(eyes)
    selected = []
    for index, name in EXPECTED[case]:
        count = samples(case, index)
        for sample in range(count):
            matches = [e for e in complete if e[0]["step"] == name and e[0]["sample"] == sample and (e[0]["ticket"] & 0xffff) == index + 1]
            require(matches, "missing stereo motion sample: " + name + "/" + str(sample))
            eyes = max(matches, key=lambda e: e[0]["app_frame"])
            progress = eyes[0]["progress"]
            require(progress >= 1 - 1e-6 if count == 1 or sample == count - 1 else abs(progress - sample / (count - 1)) <= .20, "missed motion sampling window")
            selected.append(eyes)
    require(len(complete) == len(selected), "unexpected/duplicate completed motion samples")
    baseline = selected[0][0]
    require(all((r["ticket"] >> 16) == (baseline["ticket"] >> 16) for r in capture_rows), "mixed run serials")
    for previous, current in zip(selected, selected[1:]):
        a, b = previous[0], current[0]
        require(b["app_frame"] > a["app_frame"] and b["sequence"] > a["sequence"] and b["display_time_ns"] > a["display_time_ns"], "nonmonotonic sampled frames/XR timestamps")
        if a["step"] == b["step"]:
            require(b["progress"] - a["progress"] >= .05, "repeated endpoint is not intermediate motion evidence")
    for eyes in selected:
        r = eyes[0]
        require(r["rig"] == baseline["rig"] and r["generation"] == baseline["generation"] and r["context"]["space"] == baseline["context"]["space"], "run crossed rig/input/reference identity")
        for key in ("world_scale", "height_offset", "render_scale", "steady_camera", "fov_mode", "cull_mode", "overscan"):
            require(r["context"][key] == baseline["context"][key], "render policy changed during run: " + key)
        if case != "weapon-swap":
            require(r["owner"] == baseline["owner"] and r["weapon"] == baseline["weapon"], "unexpected native owner/weapon transition")
        if case == "tracking-loss":
            if r["step"] == "right-tracking-lost":
                require(r["tracked"] == 0 and r["authority"] == 0 and r["hand_epoch"] != baseline["hand_epoch"], "tracking loss was not observed")
            elif r["step"] == "tracked-trigger-held":
                require(r["tracked"] == 1 and r["authority"] == 0, "held recovery incorrectly rearmed")
            elif r["step"] == "neutral-rearm":
                require(r["tracked"] == 1 and r["authority"] == 1, "neutral recovery did not rearm")
    if case == "weapon-swap":
        changed, settled = selected[1][0], selected[2][0]
        require(changed["weapon"] != baseline["weapon"] and changed["owner"] != baseline["owner"] and changed["owner"] == settled["owner"] and changed["weapon"] == settled["weapon"], "request did not produce a stable observed weapon/owner change")
    captures = []
    for eyes in selected:
        r = eyes[0]
        applied_clock = timing.get(r["pose_sequence"], {})
        captures.append({"step": r["step"], "sample": r["sample"], "progress": r["progress"], "left": r["file"], "right": eyes[1]["file"], "app_frame": r["app_frame"], "sequence": r["sequence"],
                         "display_time_ns": r["display_time_ns"], "source_format": r["source_format"], "context": r["context"], "rig": r["rig"], "pose_sequence": r["pose_sequence"], "pose_app_frame": r["pose_app_frame"],
                         "applied_display_time_ns": applied_clock.get("display_time_ns"), "applied_progress": applied_clock.get("progress"),
                         "pose_frame_gaps": [eyes[e]["app_frame"] - eyes[e]["pose_app_frame"] for e in (0, 1)], "palettes": [eyes[e]["palette"] for e in (0, 1)]})
    report = {"schema": 2, "case": case, "environment": begin["environment"], "mechanical": "CHECKS_PASSED_REVIEW_PENDING", "observed_complete": True,
              "provenance": p, "configuration": ini, "anchor": anchor, "stereo_pairs": len(selected), "partial_pairs": sum(set(e) != {0, 1} for e in groups.values()),
              "visual_review": "required", "headset_accepted": False, "captures": captures, "artifact_hashes": artifacts,
              "limits": "Sampled pre-DLSS game eyes and palette-writer copies; not continuous video, final composition, GPU-consumer or headset acceptance. Save/profile identities are declarations, not loaded-state proof."}
    report["report_sha256"] = digest(canonical(report))
    return report


def near(a, b, tolerance):
    return len(a) == len(b) and all(abs(x - y) <= tolerance for x, y in zip(a, b))


def same_orientation(a, b):
    dot = abs(sum(x * y for x, y in zip(a, b))) / math.sqrt(sum(x * x for x in a) * sum(y * y for y in b))
    return dot >= math.cos(math.radians(.5) / 2)


def compare(first, second, allow_changes=()):
    require(set(allow_changes) <= set(ALLOW_CHANGES), "unknown/unsafe comparison allowance")
    a, b = verify(first), verify(second)
    require(a["case"] == b["case"] and a["environment"] == b["environment"], "cannot compare different cases/environments")
    pa, pb = a["provenance"], b["provenance"]
    for key in ("target", "runtime", "fixture", "capture"):
        require(pa[key] == pb[key], "incompatible comparison provenance: " + key)
    changes = []
    if pa["mod"] != pb["mod"]:
        require("mod" in allow_changes, "changed mod binary requires --allow-change mod")
        changes.append("mod")
    for key in pa["settings"]:
        if pa["settings"][key] != pb["settings"][key]:
            require(key in allow_changes, "undeclared effective setting change: " + key)
            changes.append("settings." + key)
    ca, cb = a["configuration"], b["configuration"]
    configurable = {"armrigfix": "ArmRigFix", "armwristtwistlimit": "ArmWristTwistLimit"}
    for section in ca.keys() | cb.keys():
        sa, sb = ca.get(section, {}), cb.get(section, {})
        for key in sa.keys() | sb.keys():
            if sa.get(key) != sb.get(key):
                require(section == "vr" and configurable.get(key) in allow_changes, "undeclared INI change: " + section + "." + key)
                changes.append("config." + section + "." + key)
    require(near(a["anchor"]["position"], b["anchor"]["position"], .01), "different fixture anchors (>1 cm per axis)")
    require(same_orientation(a["anchor"]["orientation"], b["anchor"]["orientation"]), "different fixture anchor orientations (>0.5 degrees)")
    x, y = a["captures"][0], b["captures"][0]
    require(x["rig"] == y["rig"], "different avatar topology")
    require(near(x["context"]["reference"], y["context"]["reference"], .01), "different recenter frames")
    wx, wy = x["palettes"][0]["world"], y["palettes"][0]["world"]
    require(near(wx[:12], wy[:12], .01) and near(wx[12:15], wy[12:15], .02 * x["context"]["world_scale"]) and abs(wx[15] - wy[15]) <= 1e-5, "different native baseline model-to-world fixture")
    result = []
    for x, y in zip(a["captures"], b["captures"]):
        require((x["step"], x["sample"]) == (y["step"], y["sample"]), "different motion coverage")
        require(x["source_format"] == y["source_format"], "different source texture formats")
        require(abs(x["progress"] - y["progress"]) <= .025, "different sampled motion progress")
        cx, cy = x["context"], y["context"]
        for key in ("world_scale", "height_offset", "render_scale", "steady_camera", "fov_mode", "cull_mode", "overscan", "stereo", "afw", "dlss"):
            require(cx[key] == cy[key], "incompatible render policy: " + key)
        require(near(cx["fov"], cy["fov"], 1e-5), "different eye FOVs")
        ipd = lambda c: math.sqrt(sum((c["eyes"][i] - c["eyes"][7 + i]) ** 2 for i in range(3)))
        require(abs(ipd(cx) - ipd(cy)) <= .0001, "different eye separation")
        for offset in (0, 7):
            require(near(cx["eyes"][offset:offset + 3], cy["eyes"][offset:offset + 3], .01) and same_orientation(cx["eyes"][offset + 3:offset + 7], cy["eyes"][offset + 3:offset + 7]), "different sampled head/eye fixture poses")
        for eye in ("left", "right"):
            p, w, h = bitmap(Path(first) / x[eye]); q, w2, h2 = bitmap(Path(second) / y[eye])
            require((w, h) == (w2, h2), "cannot pixel-compare different dimensions")
            stride = (w * 3 + 3) & ~3
            changed = sum(p[54 + row * stride + col] != q[54 + row * stride + col] for row in range(h) for col in range(w * 3))
            result.append({"step": x["step"], "sample": x["sample"], "eye": eye, "changed_channel_fraction": changed / (w * h * 3)})
    return {"case": a["case"], "environment": a["environment"], "comparison": result, "allowed_changes": sorted(set(allow_changes)), "observed_changes": changes,
            "tolerances": {"xr_position_per_axis_m": .01, "orientation_degrees": .5, "ipd_m": .0001, "fov_radians": 1e-5, "motion_progress": .025, "native_world_translation_m": .02, "native_world_basis_component": .01},
            "baseline_report_sha256": a["report_sha256"], "candidate_report_sha256": b["report_sha256"], "visual_verdict": "not_automated", "headset_accepted": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--allow-change", action="append", choices=ALLOW_CHANGES, default=[])
    args = parser.parse_args()
    try:
        require(args.compare is not None or not args.allow_change, "allow-change requires a comparison")
        print(json.dumps(compare(args.directory, args.compare, args.allow_change) if args.compare else verify(args.directory), indent=2))
        return 0
    except FileNotFoundError as exc:
        print(str(exc)); return 2
    except (Invalid, ValueError, OSError, KeyError, TypeError, struct.error) as exc:
        print("INVALID: " + str(exc)); return 1


if __name__ == "__main__":
    raise SystemExit(main())
