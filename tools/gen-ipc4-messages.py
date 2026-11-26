#!/usr/bin/env python3
"""Generate a reviewable IPC4 capture-message plan.

This tool consumes the Raptor Lake CoSA topology and the signed SOF manifest.
It emits headers and payload templates for the two pipelines used by the DMIC
capture route.  The result is declarative and is never sent to hardware.  The
machine-specific DMIC gateway blob is intentionally represented as a required
NHLT input because macOS does not currently expose that blob to user space.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import uuid as uuidlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tplg  # noqa: E402


TOKENS = {
    "core": 404,
    "uuid": 405,
    "cpc": 406,
    "is_pages": 409,
    "num_input_pins": 411,
    "num_output_pins": 412,
    "num_input_formats": 415,
    "num_output_formats": 416,
    "stack": 420,
    "heap": 421,
    "gain_val": 262,
    "gain_ramp_type": 260,
    "gain_ramp_duration": 261,
    "node_type": 1980,
    "dai_type": 154,
    "dai_index": 155,
    "sched_lp": 207,
    "sched_priority": 201,
    "sched_direction": 211,
    "sched_core": 203,
    "sched_direction_valid": 212,
    "in_rate": 1900,
    "in_bit_depth": 1901,
    "in_valid_bits": 1902,
    "in_channels": 1903,
    "in_ch_map": 1904,
    "in_ch_cfg": 1905,
    "in_interleave": 1906,
    "in_fmt_cfg": 1907,
    "in_sample_type": 1908,
    "in_pin": 1909,
    "out_rate": 1930,
    "out_bit_depth": 1931,
    "out_valid_bits": 1932,
    "out_channels": 1933,
    "out_ch_map": 1934,
    "out_ch_cfg": 1935,
    "out_interleave": 1936,
    "out_fmt_cfg": 1937,
    "out_sample_type": 1938,
    "out_pin": 1939,
    "ibs": 1970,
    "obs": 1971,
}

CAPTURE_FORMAT = {"rate": 48000, "channels": 2, "valid_bits": 32, "sample_type": 1}


def uuid_text(raw: bytes) -> str:
    return "%s-%s-%s-%s-%s" % (
        raw[0:4][::-1].hex(), raw[4:6][::-1].hex(), raw[6:8][::-1].hex(),
        raw[8:10].hex(), raw[10:16].hex())


def values(widget: dict[str, object]) -> dict[int, list[object]]:
    out: dict[int, list[object]] = {}
    for _typ, elements in tplg.parse_arrays(widget["priv"]):
        for token, value in elements:
            out.setdefault(token, []).append(value)
    return out


def first(v: dict[int, list[object]], token: str, default: int = 0) -> int:
    got = v.get(TOKENS[token], [default])[0]
    return int(got) if not isinstance(got, str) else default


def format_groups(widget: dict[str, object], direction: str) -> list[dict[str, int]]:
    """Reconstruct repeated IPC4 audio-format tuple groups."""
    prefix = "in_" if direction == "in" else "out_"
    marker = TOKENS[prefix + "fmt_cfg"]
    groups: list[dict[str, int]] = []
    for _typ, elements in tplg.parse_arrays(widget["priv"]):
        if _typ != tplg.TUPLE_WORD:
            continue
        current: dict[str, int] = {}
        for token, value in elements:
            names = [name for name, number in TOKENS.items() if number == token]
            if names:
                current[names[0]] = int(value)
        if marker in [token for token, _value in elements]:
            groups.append(current)
    return groups


def select_format(widget: dict[str, object], direction: str) -> dict[str, int]:
    groups = format_groups(widget, direction)
    if not groups:
        raise ValueError("no %s audio formats in %s" % (direction, widget["name"]))
    for group in groups:
        if (group.get(direction + "_rate") == CAPTURE_FORMAT["rate"] and
            group.get(direction + "_channels") == CAPTURE_FORMAT["channels"] and
            group.get(direction + "_valid_bits") == CAPTURE_FORMAT["valid_bits"] and
            group.get(direction + "_sample_type") == CAPTURE_FORMAT["sample_type"]):
            return group
    raise ValueError("%s has no 48 kHz 2ch S32 format" % widget["name"])


def audio_format(group: dict[str, int], direction: str) -> list[int]:
    return [
        group.get(direction + "_rate", 0),
        group.get(direction + "_bit_depth", 0),
        group.get(direction + "_ch_map", 0),
        group.get(direction + "_ch_cfg", 0),
        group.get(direction + "_interleave", 0),
        group.get(direction + "_fmt_cfg", 0),
    ]


def base_config(widget: dict[str, object], group: dict[str, int], direction: str) -> list[int]:
    v = values(widget)
    return [
        0, group.get(direction + "_ibs", first(v, "ibs")),
        group.get(direction + "_obs", first(v, "obs")), first(v, "is_pages"),
        *audio_format(group, direction),
    ]


def manifest(path: Path, payload_offset: int) -> dict[str, dict[str, object]]:
    data = path.read_bytes()
    off = payload_offset + 0x2000
    if data[off:off + 4] != b"$AM1":
        raise ValueError("ADSP manifest not found at %#x" % off)
    header_len, = struct.unpack_from("<I", data, off + 4)
    num, = struct.unpack_from("<I", data, off + 36)
    pos = off + header_len
    entries: list[dict[str, object]] = []
    for module_id in range(num):
        if data[pos:pos + 4] != b"$AME":
            raise ValueError("bad module entry %d" % module_id)
        raw_uuid = data[pos + 12:pos + 28]
        typ, = struct.unpack_from("<I", data, pos + 28)
        cfg_offset, cfg_count = struct.unpack_from("<HH", data, pos + 68)
        instance_max, = struct.unpack_from("<H", data, pos + 76)
        entries.append({
            "id": module_id,
            "name": data[pos + 4:pos + 12].rstrip(b"\0").decode("ascii"),
            "uuid": uuid_text(raw_uuid),
            "type": typ,
            "cfg_offset": cfg_offset,
            "cfg_count": cfg_count,
            "instance_max_count": instance_max,
        })
        pos += 116
    total_cfg = max((int(e["cfg_offset"]) + int(e["cfg_count"]) for e in entries), default=0)
    configs = []
    for i in range(total_cfg):
        words = struct.unpack_from("<11I", data, pos + i * 44)
        configs.append({"ibs": words[6], "obs": words[7], "cpc": words[9]})
    result = {}
    for entry in entries:
        entry["configs"] = configs[int(entry["cfg_offset"]):int(entry["cfg_offset"]) + int(entry["cfg_count"])]
        result[str(entry["uuid"])] = entry
    return result


def choose_cpc(module: dict[str, object], ibs: int, obs: int) -> int:
    matches = [int(c["cpc"]) for c in module["configs"] if c["ibs"] == ibs and c["obs"] == obs]
    return max(matches, default=0)


def fw_module(widget: dict[str, object], modules: dict[str, dict[str, object]]) -> dict[str, object]:
    uid = None
    for _typ, elements in tplg.parse_arrays(widget["priv"]):
        for token, value in elements:
            if token == TOKENS["uuid"]:
                uid = uuid_text(value)
    if uid is None or uid not in modules:
        raise ValueError("module UUID missing from manifest for %s" % widget["name"])
    return modules[uid]


def pack_words(words: list[int]) -> list[int]:
    return [int(x) & 0xffffffff for x in words]


def message(label: str, primary: int, extension: int, payload: list[int] | None = None,
            notes: list[str] | None = None) -> dict[str, object]:
    payload = pack_words(payload or [])
    return {
        "label": label,
        "primary": primary & 0xffffffff,
        "extension": extension & 0xffffffff,
        "payload_words": payload,
        "payload_bytes": len(payload) * 4,
        "notes": notes or [],
    }


def write_binary(messages: list[dict[str, object]], path: Path) -> None:
    """Write the private I4PC replay format consumed by DSPDevice."""
    out = bytearray(struct.pack("<4sI", b"I4PC", len(messages)))
    for item in messages:
        payload = [int(word) & 0xffffffff for word in item["payload_words"]]
        length = 12 + len(payload) * 4
        if length > 4096:
            raise ValueError("IPC4 message exceeds the mailbox payload limit")
        out += struct.pack("<III", length, int(item["extension"]), int(item["primary"]))
        out += struct.pack("<%dI" % len(payload), *payload) if payload else b""
    path.write_bytes(out)


def coefficient_config_message(widget: dict[str, object], module_id: int,
                               instance_id: int, name: str) -> dict[str, object]:
    """Send topology coefficients, excluding the 32-byte SOF ABI wrapper.

    LARGE_CONFIG_SET uses a byte count, unlike INIT_INSTANCE's DWORD count.
    Accept only single-fragment, parameter-0 topology coefficient blobs.
    """
    controls = [c for c in widget["kcontrols"] if c["type"] == 2]
    if len(controls) != 1:
        raise ValueError(name + " must have exactly one bytes control")
    blob = controls[0]["priv"]
    if len(blob) < 32:
        raise ValueError("truncated " + name + " SOF ABI header")
    magic, param_id, size, abi, *reserved = struct.unpack_from("<8I", blob)
    if magic != 0x34464f53 or param_id != 0 or any(reserved):
        raise ValueError("unexpected " + name + " SOF4 ABI header")
    if abi >> 24 != 3 or size != len(blob) - 32 or not 0 < size <= 4096 or size % 4:
        raise ValueError("invalid " + name + " ABI version or single-fragment length")
    data = blob[32:]
    if struct.unpack_from("<I", data)[0] != size:
        raise ValueError(name + " internal size differs from ABI size")
    if not 0 <= module_id <= 0xffff or not 0 <= instance_id <= 0xff:
        raise ValueError(name + " module or instance ID out of range")
    return message("LARGE_CONFIG_SET " + name + " coefficients",
                   0x40000000 | (4 << 24) | (instance_id << 16) | module_id,
                   (1 << 29) | (1 << 28) | size,
                   list(struct.unpack("<%dI" % (size // 4), data)),
                   ["SOF ABI wrapper stripped; parameter 0; first and last fragment",
                    "coefficient SHA256=" + hashlib.sha256(data).hexdigest()])


def tdfb_config_message(widget: dict[str, object], module_id: int,
                        instance_id: int) -> dict[str, object]:
    return coefficient_config_message(widget, module_id, instance_id, "tdfb.11.1")


def eq_config_message(widget: dict[str, object], module_id: int,
                      instance_id: int) -> dict[str, object]:
    msg = coefficient_config_message(widget, module_id, instance_id, "eqiir.12.1")
    words = msg["payload_words"]
    # SOF user/eq.h: config header (7 words), channel assignments (2),
    # response header (6), one biquad (7). Restrict this experiment to the
    # reviewed two-channel topology preset; preserve its signed coefficients.
    expected = [88, 2, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0]
    if len(words) != 22 or words[:15] != expected:
        raise ValueError("EQ preset must be 88 bytes, two channels sharing one single-biquad response")
    return msg


def resource_pages(typ: int, ibs: int, is_pages: int) -> int:
    task = 448 + 12 + is_pages
    task += ((128 if typ & 0x40 else 72) + 63) & ~63
    task += 8 * 12
    task += 12 if typ & 0x20 else 20
    queue = 8 * (128 + ((ibs + 63) & ~63))
    return (task + queue + 4095) // 4096


def module_payload(widget: dict[str, object], module: dict[str, object],
                   pipeline_id: int, instance_id: int,
                   nhlt_words: list[int] | None) -> tuple[list[int], list[str]]:
    v = values(widget)
    name = str(widget["name"])
    notes: list[str] = []
    kind = int(widget["id"])
    if kind == 17:
        raise ValueError("scheduler is handled separately")
    in_group = select_format(widget, "in")
    out_group = select_format(widget, "out")
    ibs = in_group.get("ibs", first(v, "ibs"))
    obs = out_group.get("obs", first(v, "obs"))
    base = [0, ibs, obs, first(v, "is_pages"), *audio_format(in_group, "in")]
    base[0] = choose_cpc(module, ibs, obs)
    if not base[0]:
        notes.append("manifest has no matching IBS/OBS CPC entry, Linux leaves cpc=0")
    if kind == 4:
        base += [0xffffffff, first(v, "gain_val", 0x7fffffff),
                 first(v, "gain_ramp_type"), 0,
                 first(v, "gain_ramp_duration"), 0]
    elif kind in (12, 14, 16):
        out_fmt = audio_format(out_group, "out")
        node_type = first(v, "node_type")
        if kind == 14:
            node_id = (node_type << 8) | first(v, "dai_index")
            # Linux's IPC4 path passes nhlt_specific_cfg.caps to the copier
            # and expresses its byte length in dwords.  The size word that
            # precedes caps in the ACPI NHLT record is not part of this
            # payload.  The live table for this machine supplies 3048 bytes,
            # so keep the length derived from the recovered blob.
            config_length = 0
            dma_buffer_size = 4 * obs
            if nhlt_words is None:
                notes.append("DMIC gateway config is supplied by NHLT and is not embedded")
                notes.append("required NHLT vendor blob is the selected format capability block")
            else:
                config_length = len(nhlt_words)
                if config_length == 0:
                    raise ValueError("NHLT vendor blob must not be empty")
                notes.append("DMIC gateway config injected from the validated NHLT vendor blob")
        elif kind == 12:
            node_id = (node_type << 8) | 0
            config_length = 1
            dma_buffer_size = 4 * obs
        else:
            node_id = 0xffffffff
            config_length = 1
            dma_buffer_size = 0
        base = base + out_fmt + [0, node_id, dma_buffer_size, config_length]
        if kind == 14 and nhlt_words is not None:
            base += nhlt_words
        if kind == 14 and nhlt_words is None:
            notes.append("payload config_data is omitted until NHLT is recovered")
    elif kind == 18:
        payload = base
        if int(module["type"]) & 0x100:
            # base_cfg_ext with one input and one output pin.  The 12-byte
            # reserved region is followed by two sof_ipc4_pin_format structs.
            ext = [1 | (1 << 16), 0, 0, 0]
            ext += [0, 384, *audio_format(in_group, "in")]
            ext += [0, 384, *audio_format(out_group, "out")]
            payload += ext
            notes.append("manifest init config type is BASE_CFG_WITH_EXT")
        return payload, notes
    return base, notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary-output", type=Path,
                        help="optional private I4PC replay blob for the manual kext probe")
    parser.add_argument("--payload-offset", type=lambda x: int(x, 0), default=0x9DC)
    parser.add_argument("--nhlt-vendor-blob", type=Path,
                        help="optional little-endian DMIC gateway capability blob")
    parser.add_argument("--tdfb-config", action="store_true",
                        help="append TDFB coefficients for the run-9 capture experiment")
    parser.add_argument("--eq-config", action="store_true",
                        help="append run-12 EQ coefficients after TDFB (implies --tdfb-config)")
    args = parser.parse_args()

    topology = tplg.parse(str(args.topology))
    modules = manifest(args.firmware, args.payload_offset)
    nhlt_words: list[int] | None = None
    nhlt_sha256: str | None = None
    if args.nhlt_vendor_blob:
        blob = args.nhlt_vendor_blob.read_bytes()
        if not blob or len(blob) % 4:
            raise ValueError("NHLT vendor blob must be non-empty and 4-byte aligned")
        nhlt_words = list(struct.unpack("<%dI" % (len(blob) // 4), blob))
        nhlt_sha256 = hashlib.sha256(blob).hexdigest()
    wanted = {
        "dai-copier.DMIC.dmic01.capture", "gain.12.1", "eqiir.12.1",
        "module-copier.12.2", "tdfb.11.1", "drc.11.1", "host-copier.6.capture",
    }
    widgets = {str(w["name"]): w for w in topology["widgets"] if w["name"] in wanted}
    if len(widgets) != len(wanted):
        raise ValueError("topology is missing capture widgets")

    route_map = {(r["source"], r["sink"]): r for r in topology["routes"]
                 if r["source"] in wanted and r["sink"] in wanted}
    route_pairs = []
    current = "dai-copier.DMIC.dmic01.capture"
    while current != "host-copier.6.capture":
        next_pairs = [pair for pair in route_map if pair[0] == current]
        if len(next_pairs) != 1:
            raise ValueError("capture route is not a linear graph at %s" % current)
        pair = next_pairs[0]
        route_pairs.append(pair)
        current = pair[1]

    pipeline_names = {"pipeline.11": 11, "pipeline.12": 12}
    pipelines: dict[str, dict[str, object]] = {}
    for name, pipeline_id in pipeline_names.items():
        widget = next(w for w in topology["widgets"] if w["name"] == name)
        v = values(widget)
        pipelines[str(pipeline_id)] = {
            "topology_name": name,
            "instance_id_assumption": 0 if pipeline_id == 12 else 1,
            "priority": first(v, "sched_priority"),
            "lp_mode": first(v, "sched_lp"),
            "core": first(v, "sched_core"),
            "direction": first(v, "sched_direction"),
            "heap_bytes": first(v, "heap"),
            "stack_bytes": first(v, "stack"),
        }

    # Linux sets up a capture path by walking from the DAI source toward the
    # host sink.  Each pipeline scheduler is created immediately before the
    # first widget belonging to that pipeline.  Module instance IDs are
    # allocated in this same source-to-sink order, independently per module
    # class.
    pipeline_for_widget = {
        "dai-copier.DMIC.dmic01.capture": 12,
        "gain.12.1": 12,
        "eqiir.12.1": 12,
        "module-copier.12.2": 12,
        "tdfb.11.1": 11,
        "drc.11.1": 11,
        "host-copier.6.capture": 11,
    }
    setup_order = []
    created_pipelines = set()
    route_widgets = [route_pairs[0][0]] + [sink for _source, sink in route_pairs]
    for name in route_widgets:
        pipeline_id = pipeline_for_widget[name]
        pipeline_name = "pipeline.%d" % pipeline_id
        if pipeline_id not in created_pipelines:
            setup_order.append(pipeline_name)
            created_pipelines.add(pipeline_id)
        setup_order.append(name)
    module_ids: dict[int, int] = {}
    module_instances = []
    for name in setup_order:
        if name in pipeline_names:
            continue
        widget = widgets[name]
        fw = fw_module(widget, modules)
        module_id = int(fw["id"])
        instance_id = module_ids.get(module_id, 0)
        module_ids[module_id] = instance_id + 1
        pipeline_id = pipeline_for_widget[name]
        module_instances.append((name, pipeline_id, instance_id))
    modules_out = []
    init_messages = []
    for name, pipeline_id, instance_id in module_instances:
        widget = widgets[name]
        fw = fw_module(widget, modules)
        payload, notes = module_payload(widget, fw, pipeline_id, instance_id, nhlt_words)
        mod_id = int(fw["id"])
        # IPC4 MOD_INIT_INSTANCE.param_block_size is measured in 32-bit
        # DWORDs.  ``payload`` already contains one item per DWORD, so do
        # not divide the count by four here.  Under-reporting this field
        # makes the firmware parse only a prefix of the copier config and
        # typically returns IPC4_UNAVAILABLE for the DMIC copier.
        ext = len(payload) | (pipelines[str(pipeline_id)]["instance_id_assumption"] << 16)
        ext |= first(values(widget), "core") << 24
        if not (int(fw["type"]) & 0x40):
            ext |= 0 << 28
        # SOF_IPC4_MOD_INIT_INSTANCE is opcode 0.  The module target bit is
        # bit 30, while bits 24..28 must remain clear for this command.
        primary = mod_id | (instance_id << 16) | (1 << 30)
        init_messages.append(message("MOD_INIT_INSTANCE " + name, primary, ext, payload, notes))
        modules_out.append({
            "name": name, "pipeline": pipeline_id, "instance_id": instance_id,
            "module_id": mod_id, "module_name": fw["name"], "module_uuid": fw["uuid"],
            "manifest_type": fw["type"], "payload_bytes": len(payload) * 4,
            "payload_words": payload, "notes": notes,
        })

    create_messages = {}
    for pipeline_id in (12, 11):
        pipe = pipelines[str(pipeline_id)]
        contained = [m for m in modules_out if m["pipeline"] == pipeline_id]
        mem = sum(resource_pages(int(m["manifest_type"]),
                                 int(m["payload_words"][1]),
                                 int(m["payload_words"][3])) for m in contained)
        pipe["mem_usage_pages"] = mem
        # SOF_IPC4_GLB_CREATE_PIPELINE is global message type 17.  Type 16
        # is GLB_UNLOAD_MULTIPLE_MODULES and produces FW status 15 here.
        primary = (int(pipe["priority"]) << 11) | (17 << 24) | (int(pipe["instance_id_assumption"]) << 16) | mem
        payload = [0x01000007, 0, 0, 0x00030003,
                   int(pipe["domain_id"]) if "domain_id" in pipe else 0,
                   int(pipe["stack_bytes"]), int(pipe["heap_bytes"])]
        ext = (int(pipe["core"]) << 20) | (1 << 29)
        create_messages[pipeline_id] = message(
            "GLB_CREATE_PIPELINE " + str(pipeline_id), primary, ext, payload,
            ["pipeline instance IDs follow Linux ida allocation order"])

    instance_by_name = {name: (module_id, instance_id) for name, _pipe, instance_id in module_instances
                        for module_id in [int(fw_module(widgets[name], modules)["id"])]}
    bind_messages = []
    for source, sink in route_pairs:
        src_id, src_instance = instance_by_name[source]
        dst_id, dst_instance = instance_by_name[sink]
        primary = src_id | (src_instance << 16) | (5 << 24) | (1 << 30)
        extension = dst_id | (dst_instance << 16) | (0 << 24) | (0 << 27)
        bind_messages.append(message("MOD_BIND %s -> %s" % (source, sink), primary, extension, [],
                                     ["queue IDs are 0 for this single active route pin"]))

    result = {
        "schema": "ohmydsp.ipc4-message-plan.v1",
        "hardware": {"pci": "8086:51ca", "nhlt_endpoint": "8086:ae20", "ipc": "IPC4"},
        "firmware": {"path": str(args.firmware), "payload_offset": args.payload_offset,
                      "manifest_offset": args.payload_offset + 0x2000},
        "topology": {"path": str(args.topology), "container": topology["magic"]},
        "capture": {"rate_hz": 48000, "channels": 2, "format": "s32le", "host_pcm_id": 6,
                    "routes": route_pairs},
        "pipelines": pipelines,
        "modules": modules_out,
        "messages": (
            [
                create_messages[pipeline_names[name]] if name in pipeline_names
                else init_messages[next(i for i, item in enumerate(init_messages)
                                        if item["label"] == "MOD_INIT_INSTANCE " + name)]
                for name in setup_order
            ] + bind_messages
        ),
        "runtime_gates": {
            "hardware_writes_enabled": False,
            "firmware_download_enabled": False,
            "capture_dma_enabled": False,
            "nhlt_vendor_blob_required": nhlt_words is None,
            "nhlt_vendor_blob_bytes": len(nhlt_words) * 4 if nhlt_words is not None else None,
            "nhlt_vendor_blob_sha256": nhlt_sha256,
            "mailbox_transport_implemented": False,
        },
    }
    if args.tdfb_config or args.eq_config:
        tdfb = next(m for m in modules_out if m["name"] == "tdfb.11.1")
        result["messages"].append(tdfb_config_message(
            widgets["tdfb.11.1"], int(tdfb["module_id"]), int(tdfb["instance_id"])))
    if args.eq_config:
        eq = next(m for m in modules_out if m["name"] == "eqiir.12.1")
        result["messages"].append(eq_config_message(
            widgets["eqiir.12.1"], int(eq["module_id"]), int(eq["instance_id"])))
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.binary_output:
        write_binary(result["messages"], args.binary_output)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
