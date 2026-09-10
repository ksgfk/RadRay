#!/usr/bin/env python3
"""Generate C++ POD headers from fork-DXC schema 8 metadata blobs.

Does not invoke DXC. Feed it compiled .dxil.bin / .spirv.bin artifacts from
radray_shader_compile. Matching DXIL and SPIR-V type payloads are required.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


MAGIC = 0x59524452
SCHEMA = 8
ENVELOPE_SIZE = 152
TYPE_STRIDE = 52
NO_PARENT = 0xFFFFFFFF
NO_TYPE = 0xFFFFFFFF

KIND_SCALAR = 1
KIND_VECTOR = 2
KIND_MATRIX = 3
KIND_STRUCT = 4
KIND_ARRAY = 5

SCALAR_NONE = 0
SCALAR_FLOAT = 1
SCALAR_SINT = 2
SCALAR_UINT = 3
SCALAR_BOOL = 4


def die(message: str) -> None:
    raise SystemExit(message)


def read_name(blob: bytes, offset: int, size: int) -> str:
    if offset + size > len(blob):
        die("type name range is outside the metadata blob")
    return blob[offset : offset + size].decode("utf-8")


def parse_types(blob: bytes, source: str) -> list[dict]:
    if len(blob) < ENVELOPE_SIZE:
        die(f"{source}: metadata is smaller than a schema 8 envelope")
    magic, schema, header_size = struct.unpack_from("<IHH", blob, 0)
    if magic != MAGIC or schema != SCHEMA or header_size != ENVELOPE_SIZE:
        die(f"{source}: expected RDRY schema {SCHEMA}, got magic={magic:#x} schema={schema}")
    type_offset, type_bytes = struct.unpack_from("<II", blob, 32)
    if type_bytes % TYPE_STRIDE != 0 or type_offset + type_bytes > len(blob):
        die(f"{source}: type records are truncated or misaligned")
    records = []
    count = type_bytes // TYPE_STRIDE
    for index in range(count):
        base = type_offset + index * TYPE_STRIDE
        name_off, name_size, parent, kind, elements, offset, size, stride, flags, type_index, scalar, rows, cols = struct.unpack_from(
            "<13I", blob, base
        )
        records.append(
            {
                "name": read_name(blob, name_off, name_size),
                "parent": parent,
                "kind": kind,
                "count": elements,
                "offset": offset,
                "size": size,
                "stride": stride,
                "flags": flags,
                "type_index": type_index,
                "scalar": scalar,
                "rows": rows,
                "cols": cols,
            }
        )
    return records


def payload_key(record: dict, nested_name: str) -> tuple:
    return (
        record["name"],
        record["kind"],
        record["count"],
        record["offset"],
        record["size"],
        record["stride"],
        record["flags"],
        record["scalar"],
        record["rows"],
        record["cols"],
        nested_name,
    )


def nested_name(records: list[dict], record: dict) -> str:
    index = record["type_index"]
    if index == NO_TYPE or index >= len(records):
        return ""
    return records[index]["name"]


def collect_structs(records: list[dict]) -> dict[str, dict]:
    structs: dict[str, dict] = {}
    for index, record in enumerate(records):
        if record["parent"] != NO_PARENT or record["kind"] != KIND_STRUCT:
            continue
        fields = [child for child in records if child["parent"] == index]
        fields.sort(key=lambda item: item["offset"])
        structs[record["name"]] = {
            "name": record["name"],
            "size": record["size"],
            "fields": fields,
            "keys": tuple(payload_key(field, nested_name(records, field)) for field in fields),
            "root_key": (
                record["kind"],
                record["count"],
                record["offset"],
                record["size"],
                record["stride"],
                record["flags"],
                record["scalar"],
                record["rows"],
                record["cols"],
            ),
        }
    return structs


def merge_structs(target: dict[str, dict], source: dict[str, dict], label: str) -> None:
    for name, struct in source.items():
        existing = target.get(name)
        if existing is None:
            target[name] = struct
            continue
        if existing["root_key"] != struct["root_key"] or existing["keys"] != struct["keys"]:
            die(f"{label}: type payload for '{name}' does not match across metadata blobs")


def leaf_type(record: dict) -> str:
    scalar = record["scalar"]
    rows = record["rows"]
    cols = record["cols"]
    kind = record["kind"]
    if kind == KIND_MATRIX or rows > 1:
        if scalar == SCALAR_FLOAT and rows == 4 and cols == 4:
            return "float4x4"
        die(f"unsupported matrix {record['name']}: {rows}x{cols} scalar={scalar}")
    if kind == KIND_VECTOR or (kind == KIND_ARRAY and cols > 1 and rows <= 1):
        if scalar == SCALAR_FLOAT and cols in (2, 3, 4):
            return f"float{cols}"
        die(f"unsupported vector {record['name']}: cols={cols} scalar={scalar}")
    if kind == KIND_ARRAY:
        if scalar == SCALAR_FLOAT:
            return "float"
        if scalar == SCALAR_SINT:
            return "int32_t"
        if scalar in (SCALAR_UINT, SCALAR_BOOL):
            return "uint32_t"
        die(f"unsupported array element {record['name']}: scalar={scalar}")
    if scalar == SCALAR_FLOAT:
        return "float"
    if scalar == SCALAR_SINT:
        return "int32_t"
    if scalar in (SCALAR_UINT, SCALAR_BOOL):
        return "uint32_t"
    die(f"unsupported scalar {record['name']}: kind={kind} scalar={scalar}")


def field_decl(record: dict, nested: str, pad_index: list[int]) -> list[str]:
    lines: list[str] = []
    if record["kind"] == KIND_STRUCT:
        if not nested:
            die(f"struct field '{record['name']}' has no nested type")
        lines.append(f"    {nested} {record['name']};")
        return lines
    if record["kind"] == KIND_ARRAY:
        if nested:
            lines.append(f"    {nested} {record['name']}[{record['count']}];")
        else:
            lines.append(f"    {leaf_type(record)} {record['name']}[{record['count']}];")
        return lines
    lines.append(f"    {leaf_type(record)} {record['name']};")
    return lines


def emit_struct(name: str, struct: dict, prefix: str) -> str:
    ident = name if name.startswith(prefix) else name
    lines = [f"struct {ident} {{"]
    cursor = 0
    pad = [0]
    for field in struct["fields"]:
        if field["offset"] < cursor:
            die(f"{name}.{field['name']} overlaps previous field")
        if field["offset"] > cursor:
            gap = field["offset"] - cursor
            lines.append(f"    uint8_t _pad{pad[0]}[{gap}];")
            pad[0] += 1
            cursor = field["offset"]
        nested = ""
        if field["type_index"] != NO_TYPE:
            nested = field.get("nested", "")
        lines.extend(field_decl(field, nested, pad))
        if field["kind"] == KIND_ARRAY:
            cursor = field["offset"] + field["stride"] * field["count"]
        elif field["kind"] == KIND_STRUCT:
            cursor = field["offset"] + field["size"]
        else:
            sizes = {
                "float": 4,
                "int32_t": 4,
                "uint32_t": 4,
                "float2": 8,
                "float3": 12,
                "float4": 16,
                "float4x4": 64,
            }
            cursor = field["offset"] + sizes[leaf_type(field)]
    if cursor < struct["size"]:
        lines.append(f"    uint8_t _pad{pad[0]}[{struct['size'] - cursor}];")
    elif cursor > struct["size"]:
        die(f"{name} generated size {cursor} exceeds wire size {struct['size']}")
    lines.append("};")
    lines.append(f"static_assert(sizeof({ident}) == {struct['size']});")
    lines.append(f"static_assert(std::is_standard_layout_v<{ident}> && std::is_trivially_copyable_v<{ident}>);")
    return "\n".join(lines)


def attach_nested_names(structs: dict[str, dict], records_by_blob: list[list[dict]]) -> None:
    for records in records_by_blob:
        for struct in structs.values():
            for field in struct["fields"]:
                if field["type_index"] != NO_TYPE:
                    field["nested"] = nested_name(records, field)


def topological_order(structs: dict[str, dict]) -> list[str]:
    remaining = set(structs)
    ordered: list[str] = []
    while remaining:
        progress = False
        for name in sorted(remaining):
            deps = {
                field.get("nested", "")
                for field in structs[name]["fields"]
                if field.get("nested")
            }
            if deps <= (set(ordered) | {name}):
                ordered.append(name)
                remaining.remove(name)
                progress = True
        if not progress:
            die(f"cyclic or unresolved nested types: {', '.join(sorted(remaining))}")
    return ordered


def generate(blobs: list[tuple[str, bytes]], prefix: str) -> str:
    merged: dict[str, dict] = {}
    parsed_records: list[list[dict]] = []
    has_dxil = False
    has_spirv = False
    for path, blob in blobs:
        if blob[12] == 0:
            has_dxil = True
        elif blob[12] == 1:
            has_spirv = True
        records = parse_types(blob, path)
        parsed_records.append(records)
        merge_structs(merged, collect_structs(records), path)
    if not has_dxil or not has_spirv:
        die("inputs must include at least one DXIL blob and one SPIR-V blob")
    if not merged:
        die("no cbuffer struct types were published; the layout owner must read every field")
    # Re-attach nested names from the first blob that contains each struct.
    for records in parsed_records:
        names = collect_structs(records)
        for name, struct in names.items():
            for src, dst in zip(struct["fields"], merged[name]["fields"]):
                dst["nested"] = nested_name(records, src)
    order = topological_order(merged)
    blocks = [
        "// Generated by tools/generate_forward_cbuffers.py; do not edit.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <type_traits>",
        "",
        "#include <radray/runtime/render_framework/hlsl_math.h>",
        "",
        "namespace radray {",
        "",
    ]
    for name in order:
        ident = name if name.startswith(prefix) or prefix == "" else name
        blocks.append(emit_struct(ident, merged[name], prefix))
        blocks.append("")
    blocks.append("}  // namespace radray")
    blocks.append("")
    return "\n".join(blocks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("blobs", nargs="+", type=Path, help="schema 8 metadata blobs")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prefix", default="Forward_")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    blobs: list[tuple[str, bytes]] = []
    for path in args.blobs:
        if not path.is_file():
            die(f"missing blob: {path}")
        blobs.append((str(path), path.read_bytes()))
    content = generate(blobs, args.prefix)
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != content:
            die(f"generated header differs from {args.output}; regenerate it")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
