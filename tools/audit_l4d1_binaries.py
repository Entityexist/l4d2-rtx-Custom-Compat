#!/usr/bin/env python3
"""Offline PE/signature audit for the isolated Left 4 Dead 1 bootstrap.

No third-party packages are required. The tool maps PE32 sections to their RVAs,
extracts common Source interface strings and compares L4D2 address signatures
against the submitted L4D1 modules. It never executes or modifies the binaries.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
from typing import Any

MODULE_KEYS = {
    "engine": "ENGINE_MOD",
    "client": "CLIENT_MOD",
    "shaderapidx9": "RENDERER_MOD",
    "server": "SERVER_MOD",
    "studiorender": "STUDIORENDER_MOD",
}
INTERFACE_RE = re.compile(
    rb"(?:VClient|VEngineClient|VEngineEffects|EngineTraceClient|VClientEntityList|"
    rb"VEngineCvar|VModelInfoClient|VGUI_Surface|ShaderApi|VMaterialSystem|"
    rb"PlayerInfoManager|VSERVERTOOLS)\d{3}"
)


def parse_pe(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise ValueError(f"{path}: not an MZ executable")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError(f"{path}: invalid PE header")

    machine, section_count, timestamp, _, _, optional_size, characteristics = struct.unpack_from(
        "<HHIIIHH", data, pe_offset + 4
    )
    optional_offset = pe_offset + 24
    if optional_offset + optional_size > len(data):
        raise ValueError(f"{path}: truncated optional header")
    magic = struct.unpack_from("<H", data, optional_offset)[0]
    if magic != 0x10B:
        raise ValueError(f"{path}: expected PE32 (0x10B), got 0x{magic:04X}")

    entry_rva = struct.unpack_from("<I", data, optional_offset + 16)[0]
    image_base = struct.unpack_from("<I", data, optional_offset + 28)[0]
    image_size = struct.unpack_from("<I", data, optional_offset + 56)[0]
    headers_size = struct.unpack_from("<I", data, optional_offset + 60)[0]
    subsystem = struct.unpack_from("<H", data, optional_offset + 68)[0]
    image = bytearray(image_size)
    header_copy = min(headers_size, len(data), image_size)
    image[:header_copy] = data[:header_copy]

    sections = []
    section_offset = optional_offset + optional_size
    for index in range(section_count):
        offset = section_offset + index * 40
        if offset + 40 > len(data):
            raise ValueError(f"{path}: truncated section table")
        name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, offset + 8)
        section_characteristics = struct.unpack_from("<I", data, offset + 36)[0]
        if raw_pointer < len(data) and virtual_address < image_size:
            chunk = data[raw_pointer : min(raw_pointer + raw_size, len(data))]
            chunk = chunk[: max(0, image_size - virtual_address)]
            image[virtual_address : virtual_address + len(chunk)] = chunk
        sections.append(
            {
                "name": name,
                "rva": virtual_address,
                "virtual_size": virtual_size,
                "raw_size": raw_size,
                "characteristics": f"0x{section_characteristics:08X}",
            }
        )

    interfaces = sorted({match.decode("ascii") for match in INTERFACE_RE.findall(data)})
    return {
        "filename": path.name,
        "size_bytes": len(data),
        "machine": f"0x{machine:04X}",
        "timestamp": timestamp,
        "entry_rva": f"0x{entry_rva:X}",
        "image_base": f"0x{image_base:X}",
        "image_size": image_size,
        "subsystem": subsystem,
        "characteristics": f"0x{characteristics:04X}",
        "sha1": hashlib.sha1(data).hexdigest(),
        "sha256": hashlib.sha256(data).hexdigest(),
        "sections": sections,
        "source_interfaces": interfaces,
        "_image": bytes(image),
    }


def parse_pattern(signature: str) -> list[int | None]:
    result: list[int | None] = []
    for token in signature.split():
        if "?" in token:
            result.append(None)
        else:
            result.append(int(token, 16))
    return result


def find_all(data: bytes, pattern: list[int | None]) -> list[int]:
    if not pattern:
        return []
    runs: list[tuple[int, int, bytes]] = []
    start: int | None = None
    extended = pattern + [None]
    for index, byte in enumerate(extended):
        if byte is not None and start is None:
            start = index
        if byte is None and start is not None:
            runs.append((index - start, start, bytes(x for x in pattern[start:index] if x is not None)))
            start = None
    if not runs:
        return []
    _, anchor_offset, anchor = max(runs)
    matches: list[int] = []
    cursor = 0
    while True:
        found = data.find(anchor, cursor)
        if found < 0:
            break
        base = found - anchor_offset
        if base >= 0 and base + len(pattern) <= len(data):
            if all(expected is None or data[base + index] == expected for index, expected in enumerate(pattern)):
                matches.append(base)
        cursor = found + 1
    return matches


def extract_l4d2_patterns(source: str) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for line_number, line in enumerate(source.splitlines(), 1):
        if "find_pattern" not in line and "PATTERN_OFFSET_" not in line:
            continue
        match = re.search(
            r"\b(ENGINE_MOD|CLIENT_MOD|RENDERER_MOD|SERVER_MOD|STUDIORENDER_MOD)\b.*?\"([0-9A-Fa-f? ]{5,})\"",
            line,
        )
        if not match:
            continue
        module, signature = match.groups()
        label = f"line_{line_number}"
        label_match = re.search(
            r'(?:PATTERN_OFFSET_[A-Z_]+\([^,]+,\s*([^,]+)|"([A-Za-z0-9_]+)"\s*,\s*use_pattern)', line
        )
        if label_match:
            label = (label_match.group(1) or label_match.group(2) or label).strip()
        entries.append({"line": line_number, "module": module, "label": label, "signature": signature})
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--engine", type=Path)
    parser.add_argument("--client", type=Path)
    parser.add_argument("--shaderapidx9", type=Path)
    parser.add_argument("--stdshader-dx9", type=Path)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--studiorender", type=Path)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    inputs = {
        "exe": args.exe,
        "engine": args.engine,
        "client": args.client,
        "shaderapidx9": args.shaderapidx9,
        "stdshader_dx9": args.stdshader_dx9,
        "server": args.server,
        "studiorender": args.studiorender,
    }
    parsed: dict[str, dict[str, Any]] = {}
    module_images: dict[str, bytes] = {}
    for key, path in inputs.items():
        if path is None:
            continue
        if not path.is_file():
            parser.error(f"missing input file: {path}")
        info = parse_pe(path)
        module_key = MODULE_KEYS.get(key)
        if module_key:
            module_images[module_key] = info["_image"]
        info.pop("_image", None)
        parsed[key] = info

    source_file = args.source_root / "src" / "game" / "l4d2.cpp"
    patterns = extract_l4d2_patterns(source_file.read_text(encoding="utf-8", errors="ignore"))
    pattern_results = []
    for entry in patterns:
        image = module_images.get(entry["module"])
        if image is None:
            continue
        hits = find_all(image, parse_pattern(entry["signature"]))
        pattern_results.append({**entry, "match_count": len(hits), "rvas": [f"0x{hit:X}" for hit in hits]})

    summary: dict[str, dict[str, int]] = {}
    for module in sorted(module_images):
        rows = [row for row in pattern_results if row["module"] == module]
        summary[module] = {
            "sites": len(rows),
            "unique": sum(row["match_count"] == 1 for row in rows),
            "missing": sum(row["match_count"] == 0 for row in rows),
            "ambiguous": sum(row["match_count"] > 1 for row in rows),
        }

    report = {
        "scope": "read-only offline L4D1 bootstrap audit",
        "files": parsed,
        "l4d2_signature_compatibility_summary": summary,
        "l4d2_signature_results": pattern_results,
    }
    output = json.dumps(report, indent=2, ensure_ascii=False)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output + "\n", encoding="utf-8")
    else:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
