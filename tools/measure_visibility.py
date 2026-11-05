#!/usr/bin/env python3
"""Inventory loaded LHX objects and their nominal distance-only LOD cutoffs.

Reads a native --dump-memory snapshot without executing or modifying the game.
This is not an actual-visibility report: it omits view/cell traversal policy,
coarse boxes, frustum clipping, caches, arena limits and model selector callbacks.
Use the runtime --render-stats report for observations of rendered views.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct

MEMORY_SIZE = 0x110000
DEFAULT_DS = 0x38AB
RAW_PER_FOOT = 256
FEET_PER_MILE = 5280
MAX_OBJECTS = 65536 // 0x18


class Memory:
    def __init__(self, data: bytes):
        if len(data) != MEMORY_SIZE:
            raise ValueError(f"Expected a {MEMORY_SIZE}-byte native RAM snapshot, got {len(data)}")
        self.data = data

    def read(self, seg: int, off: int, size: int) -> bytes:
        if not 0 <= seg <= 0xFFFF or not 0 <= off <= 0xFFFF or size < 0:
            raise ValueError("Invalid segmented memory address")
        if off + size > 0x10000:
            raise ValueError(f"Record crosses segment boundary at {seg:04x}:{off:04x}")
        address = seg * 16 + off
        if address + size > len(self.data):
            raise ValueError(f"Address outside RAM at {seg:04x}:{off:04x}")
        return self.data[address:address + size]

    def word(self, seg: int, off: int) -> int:
        return struct.unpack("<H", self.read(seg, off, 2))[0]

    def xyz(self, seg: int, off: int) -> tuple[int, int, int]:
        return struct.unpack("<iii", self.read(seg, off + 6, 12))


def cutoff_metrics(threshold: int) -> dict:
    """Boundary is exclusive because INC DX makes q = floor(D/65536)+1."""
    if not 0 <= threshold <= 0xFFFF:
        raise ValueError("LOD threshold must be an unsigned word")
    raw = threshold * 65536
    feet = raw / RAW_PER_FOOT
    miles = feet / FEET_PER_MILE
    return {"threshold_word": threshold,
            "l1_boundary_raw_exclusive": raw,
            "l1_boundary_feet_exclusive": feet,
            "axis_cutoff_miles": miles,
            "equal_xz_diagonal_cutoff_miles": miles / math.sqrt(2),
            "equal_xyz_diagonal_cutoff_miles": miles / math.sqrt(3)}


def nominal_lod(distance_raw: int, far: int, near: int) -> str:
    """Apply only the original two CMP/SBB operations, including word wrapping."""
    if distance_raw < 0 or not 0 <= far <= 0xFFFF or not 0 <= near <= 0xFFFF:
        raise ValueError("Invalid distance or LOD threshold")
    q = (((distance_raw & 0xFFFFFFFF) >> 16) + 1) & 0xFFFF
    selector = 1 - (far < q) - (near < q)
    return {1: "rich", 0: "simple", -1: "distance_omitted"}[selector]


def world_inventory(memory: Memory, ds: int) -> tuple[dict, list[dict]]:
    """Follow loaded cell heads/global siblings and render-enabled group children.

    Layout comes from original140CE/1705E/14796: world+4/+6 grid counts,
    +8 grid offset, +A object segment, +C global head, +E grid segment.
    Object+4 is its sibling; render-enabled groups(flags&0x20) have children at+0x18.
    Shared lists are deduplicated. An ancestry cycle is malformed, not a duplicate.
    """
    world = memory.word(ds, 0xF019)
    if not world:
        raise ValueError("No loaded world at DS:F019")
    memory.read(ds, world, 16)
    columns, rows = memory.word(ds, world + 4), memory.word(ds, world + 6)
    cells = columns * rows
    grid_off, object_seg = memory.word(ds, world + 8), memory.word(ds, world + 10)
    global_head, grid_seg = memory.word(ds, world + 12), memory.word(ds, world + 14)
    if not columns or not rows or cells > 16384:
        raise ValueError("Invalid loaded-world grid dimensions")
    memory.read(grid_seg, grid_off, cells * 4)
    heads = [memory.word(grid_seg, grid_off + i * 4) for i in range(cells)]
    heads.append(global_head)
    objects = []
    visited: set[int] = set()
    # Iterative DFS avoids Python recursion limits on long original sibling lists.
    active_path: set[int] = set()
    pending = [(head, False) for head in reversed(heads) if head]
    while pending:
        off, leaving = pending.pop()
        if leaving:
            active_path.remove(off)
            continue
        if off in active_path:
            raise ValueError(f"Cycle in object lists at {object_seg:04x}:{off:04x}")
        if off in visited:
            continue
        if len(visited) >= MAX_OBJECTS:
            raise ValueError("Object traversal exceeds segment-derived bound")
        visited.add(off)
        active_path.add(off)
        memory.read(object_seg, off, 0x18)
        model = memory.word(object_seg, off)
        flags = memory.word(object_seg, off + 2)
        sibling = memory.word(object_seg, off + 4)
        objects.append({"offset": off, "model_record": model, "flags": flags,
                        "render_enabled": bool(flags & 1), "group": bool(flags & 0x20),
                        "position_raw": memory.xyz(object_seg, off)})
        pending.append((off, True))
        if sibling:
            pending.append((sibling, False))
        if flags & 1 and flags & 0x20:
            child = memory.word(object_seg, off + 0x18)
            if child:
                pending.append((child, False))
    return {"descriptor_offset": world, "object_segment": object_seg,
            "grid_segment": grid_seg, "grid_offset": grid_off,
            "columns": columns, "rows": rows, "cell_count": cells,
            "global_head": global_head}, objects


def model_info(memory: Memory, ds: int, record: int) -> dict:
    memory.read(ds, record, 0x28)
    far = memory.word(ds, record + 0xE)
    near = memory.word(ds, record + 0x10)
    payload_seg = memory.word(ds, record + 0x22)
    payload_off = memory.word(ds, record + 0x20)
    relocation_base = memory.word(ds, record + 0x24)
    payload_size = memory.word(ds, record + 0x26)

    def model_data(pointer: int, length: int) -> bytes:
        if not pointer:
            return b""
        if payload_seg:
            relative = pointer - relocation_base
            if relative < 0 or relative + length > payload_size:
                raise ValueError(f"Model pointer outside far payload at DS:{record:04x}")
            return memory.read(payload_seg, payload_off + relative, length)
        return memory.read(ds, pointer, length)

    name_ptr = memory.word(ds, record + 0x1E)
    # Read through NUL rather than demanding13bytes after a payload's final name.
    name = bytearray()
    for i in range(13):
        ch = model_data(name_ptr + i, 1) if name_ptr else b"\0"
        if not ch or ch == b"\0":
            break
        name.extend(ch)
    display_name = name.decode("ascii", errors="replace") or f"model@{record:04x}"
    lods = {}
    for label, field in (("simple", 0x12), ("rich", 0x14)):
        pointer = memory.word(ds, record + field)
        header = model_data(pointer, 14)
        lods[label] = {"pointer": pointer, "present": bool(pointer)}
        if header:
            lods[label].update(primitive_count=header[0], vertex_count=header[1],
                               custom_draw=bool(header[13] & 1))
    callback_off = memory.word(ds, record + 0x18)
    callback_seg = memory.word(ds, record + 0x1A)
    return {"record_offset": record, "name": display_name,
            "far_payload": bool(payload_seg), "lods": lods,
            "far_cutoff": cutoff_metrics(far), "rich_cutoff": cutoff_metrics(near),
            "selector_callback": (f"{callback_seg:04x}:{callback_off:04x}"
                                  if callback_seg else None)}


def make_report(data: bytes, ds: int = DEFAULT_DS, reference: str = "player") -> dict:
    memory = Memory(data)
    if not 0 <= ds <= 0xFFFF:
        raise ValueError("DS must be an unsigned segment word")
    world, objects = world_inventory(memory, ds)
    if reference == "player":
        player_off, player_seg = memory.word(ds, 0x120), memory.word(ds, 0x122)
        if not player_seg:
            raise ValueError("No player object at DS:0120; choose --reference last-scene if appropriate")
        origin = memory.xyz(player_seg, player_off)
        reference_description = "Player object position; not necessarily the camera position"
    elif reference == "last-scene":
        origin = struct.unpack("<iii", memory.read(ds, 0xF01B, 12))
        reference_description = "Last saved scene camera; may be an auxiliary attitude/TV view"
    else:
        raise ValueError("Unknown reference position")

    models: dict[int, dict] = {}
    counts = Counter(total=len(objects))
    nominal = Counter(rich=0, simple=0, distance_omitted=0)
    for obj in objects:
        counts["render_enabled" if obj["render_enabled"] else "render_disabled"] += 1
        if obj["group"]:
            counts["groups"] += 1
            counts["render_enabled_groups"] += obj["render_enabled"]
            continue
        counts["ordinary_objects"] += 1
        if not obj["render_enabled"]:
            continue
        counts["render_enabled_ordinary_objects"] += 1
        record = obj["model_record"]
        if record not in models:
            models[record] = model_info(memory, ds, record)
            models[record].update(render_enabled_objects=0,
                                  nominal_distance_only_counts={key: 0 for key in nominal},
                                  wrapped_distance_cases=0)
        model = models[record]
        distance = sum(abs(a - b) for a, b in zip(obj["position_raw"], origin))
        category = nominal_lod(distance, model["far_cutoff"]["threshold_word"],
                               model["rich_cutoff"]["threshold_word"])
        model["render_enabled_objects"] += 1
        model["nominal_distance_only_counts"][category] += 1
        model["wrapped_distance_cases"] += distance >= 0xFFFF0000
        nominal[category] += 1

    count_keys = ("total", "render_enabled", "render_disabled", "groups", "render_enabled_groups",
                  "ordinary_objects", "render_enabled_ordinary_objects")
    return {"schema_version": 1,
            "snapshot": {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                         "ds": ds},
            "scope": "Loaded world linked-list inventory and nominal distance-only LOD tests; not actual visibility",
            "units": {"raw_coordinate_units_per_foot": RAW_PER_FOOT,
                      "feet_per_mile": FEET_PER_MILE,
                      "evidence": "Original2103C/09BA3 range formatter: raw>>8 feet, 5280 feet per mile"},
            "reference": {"kind": reference, "description": reference_description,
                          "position_raw": list(origin),
                          "position_feet": [value / RAW_PER_FOOT for value in origin]},
            "world": world, "object_counts": {key: counts[key] for key in count_keys},
            "nominal_distance_only_counts": dict(nominal),
            "models": [models[key] for key in sorted(models)],
            "limitations": [
                "Only loaded cell/global linked lists and children of render-enabled groups are included; separate dynamic/view-cache objects may be absent.",
                "Render-enabled means object flags&1; this is not simulation activity, visibility or an enemy count. First-person ownship can have this bit temporarily cleared.",
                "Groups are counted separately and excluded from ordinary-model distance tests.",
                "Nominal tests omit cell-selection policy, coarse boxes, frustum, occlusion, caches and arena limits.",
                "Selector callbacks can override the nominal distance-only choice; missing/custom LODs may not draw ordinary geometry.",
                "Cutoff miles are exclusive boundary estimates for the L1 metric, not a spherical view radius.",
                "Original32-bit distance and16-bit q wrapping is retained; wrapped_distance_cases marks nonphysical wrap cases.",
                "No final-RAM renderer counters are attributed to the main view; use runtime --render-stats."]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ram", type=Path, help="Native --dump-memory snapshot")
    parser.add_argument("--ds", type=lambda value: int(value, 0), default=DEFAULT_DS,
                        help="Main data segment (default 0x38ab)")
    parser.add_argument("--reference", choices=("player", "last-scene"), default="player")
    parser.add_argument("--output", type=Path, help="Write JSON here instead of stdout")
    args = parser.parse_args()
    try:
        if args.ram.stat().st_size != MEMORY_SIZE:
            raise ValueError(f"Expected a {MEMORY_SIZE}-byte native RAM snapshot")
        report = make_report(args.ram.read_bytes(), args.ds, args.reference)
        report["snapshot"]["path"] = str(args.ram)
        output = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if args.output:
            if (args.output.resolve() == args.ram.resolve() or
                    args.output.exists() and args.output.samefile(args.ram)):
                raise ValueError("Output must differ from the RAM snapshot")
            args.output.write_text(output)
        else:
            print(output, end="")
    except (OSError, ValueError, struct.error) as error:
        parser.exit(2, f"measure_visibility: {error}\n")


if __name__ == "__main__":
    main()
