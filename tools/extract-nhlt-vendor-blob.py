#!/usr/bin/env python3
"""Extract a DMIC endpoint capability blob from an ACPI NHLT table.

The Raptor Lake IPC4 DMIC gateway consumes the machine-specific capability
bytes carried by a matching NHLT format configuration for vendor/device
8086:ae20. This tool only parses a saved table and writes the selected format
configuration capability block. It never accesses firmware or hardware
directly.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


TABLE_HEADER_SIZE = 36
ENDPOINT_HEADER_SIZE = 19
VENDOR_ID = 0x8086
DEVICE_ID = 0xAE20
WAVE_FORMAT_SIZE = 40


class NHLTError(ValueError):
    pass


def need(data: bytes, offset: int, size: int, label: str) -> None:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise NHLTError(
            f"{label} exceeds table bounds at 0x{offset:x} size 0x{size:x}"
        )


def u8(data: bytes, offset: int, label: str) -> int:
    need(data, offset, 1, label)
    return data[offset]


def u16(data: bytes, offset: int, label: str) -> int:
    need(data, offset, 2, label)
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int, label: str) -> int:
    need(data, offset, 4, label)
    return struct.unpack_from("<I", data, offset)[0]


def parse_endpoints(data: bytes) -> list[dict[str, object]]:
    if len(data) < TABLE_HEADER_SIZE + 1 or data[:4] != b"NHLT":
        raise NHLTError("input is not an NHLT table")
    declared_length = u32(data, 4, "table length")
    if declared_length < TABLE_HEADER_SIZE + 1 or declared_length > len(data):
        raise NHLTError(
            f"invalid table length 0x{declared_length:x} for {len(data)} bytes"
        )
    data = data[:declared_length]
    count = u8(data, TABLE_HEADER_SIZE, "endpoint count")
    offset = TABLE_HEADER_SIZE + 1
    endpoints: list[dict[str, object]] = []
    for index in range(count):
        need(data, offset, ENDPOINT_HEADER_SIZE, f"endpoint {index} header")
        length = u32(data, offset, f"endpoint {index} length")
        if length < ENDPOINT_HEADER_SIZE:
            raise NHLTError(f"endpoint {index} has invalid length {length}")
        need(data, offset, length, f"endpoint {index}")
        # Keep table-relative offsets for reports while bounding every nested read.
        bounded = memoryview(data)[:offset + length]
        endpoint = {
            "index": index,
            "offset": offset,
            "length": length,
            "link_type": u8(bounded, offset + 4, f"endpoint {index} link type"),
            "instance_id": u8(bounded, offset + 5, f"endpoint {index} instance"),
            "vendor_id": u16(bounded, offset + 6, f"endpoint {index} vendor"),
            "device_id": u16(bounded, offset + 8, f"endpoint {index} device"),
            "revision_id": u16(bounded, offset + 10, f"endpoint {index} revision"),
            "subsystem_id": u32(bounded, offset + 12, f"endpoint {index} subsystem"),
            "device_type": u8(bounded, offset + 16, f"endpoint {index} type"),
            "direction": u8(bounded, offset + 17, f"endpoint {index} direction"),
            "virtual_bus_id": u8(bounded, offset + 18, f"endpoint {index} bus"),
        }
        config_offset = offset + ENDPOINT_HEADER_SIZE
        device_capabilities_size = u32(bounded, config_offset, f"endpoint {index} device capabilities size")
        device_capabilities_offset = config_offset + 4
        need(bounded, device_capabilities_offset, device_capabilities_size,
             f"endpoint {index} device capabilities")
        endpoint["device_capabilities_offset"] = device_capabilities_offset
        endpoint["device_capabilities_size"] = device_capabilities_size
        endpoint["formats_offset"] = device_capabilities_offset + device_capabilities_size
        formats_count_offset = device_capabilities_offset + device_capabilities_size
        formats_count = u8(bounded, formats_count_offset, f"endpoint {index} format count")
        format_offset = formats_count_offset + 1
        formats: list[dict[str, object]] = []
        for format_index in range(formats_count):
            need(bounded, format_offset, WAVE_FORMAT_SIZE + 4,
                 f"endpoint {index} format {format_index} header")
            values = struct.unpack_from("<HHIIHHHHI16s", bounded, format_offset)
            _tag, channels, rate, _avg, _align, bits, _extra, valid_bits, _mask, _subformat = values
            capability_size_offset = format_offset + WAVE_FORMAT_SIZE
            capability_size = u32(bounded, capability_size_offset,
                                  f"endpoint {index} format {format_index} capability size")
            capability_offset = capability_size_offset + 4
            need(bounded, capability_offset, capability_size,
                 f"endpoint {index} format {format_index} capabilities")
            formats.append({
                "index": format_index,
                "offset": format_offset,
                "channels": channels,
                "rate": rate,
                "bits_per_sample": bits,
                "valid_bits_per_sample": valid_bits,
                "capabilities_offset": capability_offset,
                "capabilities_size": capability_size,
                "capabilities_sha256": hashlib.sha256(
                    bounded[capability_offset:capability_offset + capability_size]
                ).hexdigest(),
            })
            format_offset = capability_offset + capability_size
        endpoint["formats"] = formats
        endpoints.append(endpoint)
        offset += length
    return endpoints


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path, help="saved NHLT binary table")
    parser.add_argument("output", type=Path, help="output format capability blob")
    parser.add_argument(
        "--vendor-id", type=lambda value: int(value, 0), default=VENDOR_ID
    )
    parser.add_argument(
        "--device-id", type=lambda value: int(value, 0), default=DEVICE_ID
    )
    parser.add_argument("--format-index", type=int,
                        help="format index within the matching endpoint")
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--bits-per-sample", type=int, default=32)
    parser.add_argument("--report", type=Path, help="optional endpoint JSON report")
    args = parser.parse_args()

    data = args.table.read_bytes()
    endpoints = parse_endpoints(data)
    matches = [
        endpoint
        for endpoint in endpoints
        if endpoint["vendor_id"] == args.vendor_id
        and endpoint["device_id"] == args.device_id
    ]
    if not matches:
        raise NHLTError(
            f"no NHLT endpoint for {args.vendor_id:04x}:{args.device_id:04x}"
        )
    if len(matches) != 1:
        raise NHLTError(f"found {len(matches)} matching DMIC endpoints, select one explicitly")
    endpoint = matches[0]
    formats = list(endpoint["formats"])
    if args.format_index is not None:
        selected = [f for f in formats if f["index"] == args.format_index]
    else:
        selected = [f for f in formats
                    if f["rate"] == args.rate
                    and f["channels"] == args.channels
                    and f["bits_per_sample"] == args.bits_per_sample]
    if len(selected) != 1:
        raise NHLTError("format selection is ambiguous or missing")
    fmt = selected[0]
    size = int(fmt["capabilities_size"])
    if size % 4:
        raise NHLTError(f"matching format capability block is not word-aligned: {size} bytes")
    offset = int(fmt["capabilities_offset"])
    blob = data[offset:offset + size]
    args.output.write_bytes(blob)
    endpoint_report = {
        **endpoint,
        "selected_format": fmt,
        "vendor_id": f"0x{int(endpoint['vendor_id']):04x}",
        "device_id": f"0x{int(endpoint['device_id']):04x}",
        "subsystem_id": f"0x{int(endpoint['subsystem_id']):08x}",
        "output": str(args.output),
        "output_sha256": hashlib.sha256(blob).hexdigest(),
    }
    if args.report:
        args.report.write_text(
            json.dumps({"table": str(args.table), "endpoints": endpoints,
                        "selected": endpoint_report}, indent=2) + "\n"
        )
    print(json.dumps(endpoint_report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, NHLTError) as exc:
        raise SystemExit(f"error: {exc}")
