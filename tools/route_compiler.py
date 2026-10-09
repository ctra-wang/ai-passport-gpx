#!/usr/bin/env python3
"""Compile GPX tracks into the compact FAPR route package used by CTRA Passport."""

from __future__ import annotations

import argparse
import json
import math
import struct
import wave
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable
import xml.etree.ElementTree as ET

HEADER_SIZE = 96
POINT_SIZE = 12
MARKER_SIZE = 16
CUE_SIZE = 28
VERSION = 1
MAX_POINTS = 20000
MAX_MARKERS = 1024
MAX_CUES = 2048
MAX_ROUTE_BYTES = 0x280000

MARKER_START = 1
MARKER_FINISH = 2
MARKER_CP = 3
MARKER_KM = 4
MARKER_POI = 5

CUE_TYPES = {
    "left": 1,
    "right": 2,
    "fork": 3,
    "cp": 4,
    "climb": 5,
    "descent": 6,
    "danger": 7,
    "supply": 8,
    "finish": 9,
    "off_route": 10,
}
AUDIO_NONE = 0
AUDIO_IMA_ADPCM4 = 1

HEADER_STRUCT = struct.Struct("<4sHHIIIIIHHIIIIiiii32s")
POINT_STRUCT = struct.Struct("<iiI")
MARKER_STRUCT = struct.Struct("<iiIBBH")
CUE_STRUCT = struct.Struct("<IHBBiiIIHBB")
assert HEADER_STRUCT.size == HEADER_SIZE
assert POINT_STRUCT.size == POINT_SIZE
assert MARKER_STRUCT.size == MARKER_SIZE
assert CUE_STRUCT.size == CUE_SIZE


@dataclass
class TrackPoint:
    lat: float
    lon: float
    ele: float | None
    route_m: float


@dataclass
class Marker:
    lat_e7: int
    lon_e7: int
    route_m: int
    marker_type: int
    marker_id: int = 0
    flags: int = 0


@dataclass
class Cue:
    route_m: int
    cue_type: int
    trigger_before_m: int
    priority: int
    lat_e7: int
    lon_e7: int
    audio: bytes = b""
    sample_rate: int = 0


IMA_INDEX_TABLE = (
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
)
IMA_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
)


def haversine_m(a: tuple[float, float], b: tuple[float, float]) -> float:
    lat1, lon1 = map(math.radians, a)
    lat2, lon2 = map(math.radians, b)
    dlat = lat2 - lat1
    dlon = lon2 - lon1
    value = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 6371000.0 * 2.0 * math.atan2(math.sqrt(value), math.sqrt(max(0.0, 1.0 - value)))


def parse_gpx(path: Path) -> tuple[list[TrackPoint], list[tuple[float, float, str]]]:
    root = ET.parse(path).getroot()
    raw: list[tuple[float, float, float | None]] = []
    for element in root.iter():
        if element.tag.rsplit("}", 1)[-1] != "trkpt":
            continue
        lat = float(element.attrib["lat"])
        lon = float(element.attrib["lon"])
        ele = None
        for child in element:
            if child.tag.rsplit("}", 1)[-1] == "ele" and child.text:
                try:
                    ele = float(child.text)
                except ValueError:
                    pass
                break
        raw.append((lat, lon, ele))
    if len(raw) < 2:
        raise ValueError("GPX must contain at least two track points")

    points: list[TrackPoint] = []
    total = 0.0
    previous = raw[0]
    points.append(TrackPoint(previous[0], previous[1], previous[2], 0.0))
    for item in raw[1:]:
        total += haversine_m((previous[0], previous[1]), (item[0], item[1]))
        points.append(TrackPoint(item[0], item[1], item[2], total))
        previous = item

    waypoints: list[tuple[float, float, str]] = []
    for element in root.iter():
        if element.tag.rsplit("}", 1)[-1] != "wpt":
            continue
        name = ""
        for child in element:
            if child.tag.rsplit("}", 1)[-1] == "name" and child.text:
                name = child.text.strip()
                break
        waypoints.append((float(element.attrib["lat"]), float(element.attrib["lon"]), name))
    return points, waypoints


def resample(points: list[TrackPoint], spacing_m: float) -> list[TrackPoint]:
    if spacing_m <= 0:
        raise ValueError("sample spacing must be positive")
    if len(points) < 2:
        return list(points)

    selected = [points[0]]
    next_m = spacing_m
    total_m = points[-1].route_m
    while next_m < total_m:
        selected.append(coordinate_at(points, next_m))
        next_m += spacing_m
    if selected[-1].route_m < total_m:
        selected.append(points[-1])
    return selected


def coordinate_at(points: list[TrackPoint], route_m: float) -> TrackPoint:
    if route_m <= 0:
        return points[0]
    if route_m >= points[-1].route_m:
        return points[-1]
    lo = 0
    hi = len(points) - 1
    while lo + 1 < hi:
        mid = (lo + hi) // 2
        if points[mid].route_m < route_m:
            lo = mid
        else:
            hi = mid
    a = points[lo]
    b = points[hi]
    span = b.route_m - a.route_m
    t = 0.0 if span <= 0 else (route_m - a.route_m) / span
    ele = None
    if a.ele is not None and b.ele is not None:
        ele = a.ele + (b.ele - a.ele) * t
    return TrackPoint(
        a.lat + (b.lat - a.lat) * t,
        a.lon + (b.lon - a.lon) * t,
        ele,
        route_m,
    )


def nearest_route_m(points: list[TrackPoint], lat: float, lon: float) -> float:
    best = min(points, key=lambda p: haversine_m((lat, lon), (p.lat, p.lon)))
    return best.route_m


def build_markers(points: list[TrackPoint], waypoints: list[tuple[float, float, str]]) -> list[Marker]:
    result = [
        Marker(round(points[0].lat * 1e7), round(points[0].lon * 1e7), 0, MARKER_START),
    ]
    km = 1
    while km * 1000 < points[-1].route_m:
        p = coordinate_at(points, km * 1000.0)
        result.append(Marker(round(p.lat * 1e7), round(p.lon * 1e7), km * 1000, MARKER_KM, km))
        km += 1

    cp_id = 1
    for lat, lon, name in waypoints:
        route_m = int(round(nearest_route_m(points, lat, lon)))
        upper = name.upper()
        marker_type = MARKER_CP if upper.startswith("CP") or "CHECK" in upper else MARKER_POI
        marker_id = cp_id if marker_type == MARKER_CP else 0
        if marker_type == MARKER_CP:
            cp_id += 1
        result.append(Marker(round(lat * 1e7), round(lon * 1e7), route_m, marker_type, marker_id))

    result.append(
        Marker(
            round(points[-1].lat * 1e7),
            round(points[-1].lon * 1e7),
            int(round(points[-1].route_m)),
            MARKER_FINISH,
        )
    )
    result.sort(key=lambda marker: marker.route_m)
    return result


def bearing_deg(a: TrackPoint, b: TrackPoint) -> float:
    lat1 = math.radians(a.lat)
    lat2 = math.radians(b.lat)
    dlon = math.radians(b.lon - a.lon)
    x = math.sin(dlon) * math.cos(lat2)
    y = math.cos(lat1) * math.sin(lat2) - math.sin(lat1) * math.cos(lat2) * math.cos(dlon)
    return (math.degrees(math.atan2(x, y)) + 360.0) % 360.0


def signed_heading_delta(before: float, after: float) -> float:
    return (after - before + 540.0) % 360.0 - 180.0


def turn_candidates(points: list[TrackPoint], min_angle: float = 45.0, min_spacing_m: float = 120.0) -> list[dict]:
    candidates: list[dict] = []
    last_m = -1e9
    if len(points) < 5:
        return candidates
    for i in range(2, len(points) - 2):
        before = bearing_deg(points[i - 2], points[i])
        after = bearing_deg(points[i], points[i + 2])
        delta = signed_heading_delta(before, after)
        if abs(delta) < min_angle or points[i].route_m - last_m < min_spacing_m:
            continue
        candidate = {
            "route_m": int(round(points[i].route_m)),
            "lat_e7": round(points[i].lat * 1e7),
            "lon_e7": round(points[i].lon * 1e7),
            "turn_angle_deg": round(delta, 1),
            "deterministic_type": "right" if delta > 0 else "left",
            "suggested_trigger_before_m": 45,
            "jev_state": {
                "route_distance_m": int(round(points[i].route_m)),
                "turn_angle_deg": round(delta, 1),
                "distance_since_previous_candidate_m": None if not candidates else int(round(points[i].route_m - candidates[-1]["route_m"])),
                "elevation_m": points[i].ele,
            },
        }
        candidates.append(candidate)
        last_m = points[i].route_m
    return candidates


def deterministic_cues(points: list[TrackPoint], markers: list[Marker]) -> list[Cue]:
    cues: list[Cue] = []
    for candidate in turn_candidates(points):
        cue_type = CUE_TYPES[candidate["deterministic_type"]]
        cues.append(
            Cue(
                candidate["route_m"],
                cue_type,
                45,
                3,
                candidate["lat_e7"],
                candidate["lon_e7"],
            )
        )
    for marker in markers:
        if marker.marker_type == MARKER_CP:
            cues.append(Cue(marker.route_m, CUE_TYPES["cp"], 80, 4, marker.lat_e7, marker.lon_e7))
    finish = markers[-1]
    cues.append(Cue(finish.route_m, CUE_TYPES["finish"], 100, 5, finish.lat_e7, finish.lon_e7))
    cues.sort(key=lambda cue: cue.route_m)
    return cues


def read_pcm16_wav(path: Path, target_rate: int = 8000) -> tuple[list[int], int]:
    with wave.open(str(path), "rb") as source:
        channels = source.getnchannels()
        width = source.getsampwidth()
        rate = source.getframerate()
        frames = source.readframes(source.getnframes())
    if width != 2 or channels < 1:
        raise ValueError(f"{path}: only PCM16 WAV is supported")
    sample_count = len(frames) // 2
    values = list(struct.unpack("<" + "h" * sample_count, frames))
    if channels > 1:
        mono = []
        for i in range(0, len(values), channels):
            mono.append(int(sum(values[i:i + channels]) / channels))
        values = mono
    if rate != target_rate and values:
        duration = len(values) / rate
        out_count = max(1, int(round(duration * target_rate)))
        resampled = []
        for i in range(out_count):
            source_pos = i * rate / target_rate
            lo = min(int(source_pos), len(values) - 1)
            hi = min(lo + 1, len(values) - 1)
            frac = source_pos - lo
            resampled.append(int(round(values[lo] * (1.0 - frac) + values[hi] * frac)))
        values = resampled
        rate = target_rate
    return values, rate


def encode_ima_adpcm(samples: list[int]) -> bytes:
    if not samples:
        return b""
    predictor = max(-32768, min(32767, int(samples[0])))
    index = 0
    result = bytearray(struct.pack("<hBB", predictor, index, 0))
    pending = None

    for sample in samples[1:]:
        sample = max(-32768, min(32767, int(sample)))
        step = IMA_STEP_TABLE[index]
        diff = sample - predictor
        nibble = 0
        if diff < 0:
            nibble |= 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            nibble |= 4
            diff -= step
            delta += step
        if diff >= step >> 1:
            nibble |= 2
            diff -= step >> 1
            delta += step >> 1
        if diff >= step >> 2:
            nibble |= 1
            delta += step >> 2
        predictor += -delta if nibble & 8 else delta
        predictor = max(-32768, min(32767, predictor))
        index += IMA_INDEX_TABLE[nibble]
        index = max(0, min(88, index))

        if pending is None:
            pending = nibble & 0x0F
        else:
            result.append(pending | ((nibble & 0x0F) << 4))
            pending = None
    if pending is not None:
        result.append(pending)
    return bytes(result)


def load_cues_json(path: Path, points: list[TrackPoint]) -> list[Cue]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    entries = payload.get("cues", payload) if isinstance(payload, dict) else payload
    if not isinstance(entries, list):
        raise ValueError("cues JSON must be an array or an object containing a cues array")

    cues: list[Cue] = []
    for entry in entries:
        cue_name = str(entry["type"]).lower()
        if cue_name not in CUE_TYPES:
            raise ValueError(f"unknown cue type: {cue_name}")
        # off_route is a global alert asset rather than a sequential route cue.
        # Keep it in the same compact cue table so firmware can reuse the audio
        # container without adding another file section. It defaults to route 0
        # and navigation code explicitly skips it during normal cue progression.
        route_m = int(entry.get("route_m", 0) if cue_name == "off_route" else entry["route_m"])
        p = coordinate_at(points, route_m)
        audio = b""
        sample_rate = 0
        audio_path = entry.get("audio_wav")
        if audio_path:
            wav_path = (path.parent / audio_path).resolve()
            samples, sample_rate = read_pcm16_wav(wav_path)
            audio = encode_ima_adpcm(samples)
        cues.append(
            Cue(
                route_m=route_m,
                cue_type=CUE_TYPES[cue_name],
                trigger_before_m=int(entry.get("trigger_before_m", 50)),
                priority=int(entry.get("priority", 3)),
                lat_e7=int(entry.get("lat_e7", round(p.lat * 1e7))),
                lon_e7=int(entry.get("lon_e7", round(p.lon * 1e7))),
                audio=audio,
                sample_rate=sample_rate,
            )
        )
    cues.sort(key=lambda cue: cue.route_m)
    return cues


def safe_name(value: str) -> bytes:
    printable = "".join(ch if 32 <= ord(ch) < 127 else " " for ch in value)
    printable = " ".join(printable.split()) or "CTRA ROUTE"
    return printable.encode("ascii", "replace")[:31].ljust(32, b"\0")


def compile_route(
    points: list[TrackPoint],
    markers: list[Marker],
    cues: list[Cue],
    *,
    route_id: int,
    name: str,
) -> bytes:
    sampled = points
    if not 2 <= len(sampled) <= MAX_POINTS:
        raise ValueError(f"route point count must be between 2 and {MAX_POINTS}")
    if len(markers) > MAX_MARKERS:
        raise ValueError(f"marker count exceeds {MAX_MARKERS}")
    if len(cues) > MAX_CUES:
        raise ValueError(f"cue count exceeds {MAX_CUES}")

    point_bytes = b"".join(
        POINT_STRUCT.pack(round(p.lat * 1e7), round(p.lon * 1e7), int(round(p.route_m)))
        for p in sampled
    )
    marker_bytes = b"".join(
        MARKER_STRUCT.pack(m.lat_e7, m.lon_e7, m.route_m, m.marker_type, m.flags, m.marker_id)
        for m in markers
    )

    points_offset = HEADER_SIZE
    markers_offset = points_offset + len(point_bytes)
    cues_offset = markers_offset + len(marker_bytes)
    data_offset = cues_offset + len(cues) * CUE_SIZE

    audio_blob = bytearray()
    cue_bytes = bytearray()
    for cue in cues:
        audio_offset = 0
        audio_length = 0
        codec = AUDIO_NONE
        sample_rate = 0
        if cue.audio:
            audio_offset = data_offset + len(audio_blob)
            audio_length = len(cue.audio)
            codec = AUDIO_IMA_ADPCM4
            sample_rate = cue.sample_rate
            audio_blob.extend(cue.audio)
        cue_bytes.extend(
            CUE_STRUCT.pack(
                cue.route_m,
                max(0, min(65535, cue.trigger_before_m)),
                cue.cue_type,
                max(0, min(255, cue.priority)),
                cue.lat_e7,
                cue.lon_e7,
                audio_offset,
                audio_length,
                sample_rate,
                codec,
                0,
            )
        )

    total_size = data_offset + len(audio_blob)
    min_lat = min(round(p.lat * 1e7) for p in sampled)
    min_lon = min(round(p.lon * 1e7) for p in sampled)
    max_lat = max(round(p.lat * 1e7) for p in sampled)
    max_lon = max(round(p.lon * 1e7) for p in sampled)

    payload = point_bytes + marker_bytes + bytes(cue_bytes) + bytes(audio_blob)
    payload_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if total_size > MAX_ROUTE_BYTES:
        raise ValueError(
            f"compiled route is {total_size} bytes, exceeding Passport slot size {MAX_ROUTE_BYTES}"
        )

    header = HEADER_STRUCT.pack(
        b"FAPR",
        VERSION,
        HEADER_SIZE,
        total_size,
        payload_crc,
        route_id & 0xFFFFFFFF,
        int(round(sampled[-1].route_m)),
        len(sampled),
        len(markers),
        len(cues),
        points_offset,
        markers_offset,
        cues_offset,
        data_offset,
        min_lat,
        min_lon,
        max_lat,
        max_lon,
        safe_name(name),
    )
    return header + payload


def command() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gpx", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--name", help="ASCII display name used on Passport")
    parser.add_argument("--route-id", type=int, default=0)
    parser.add_argument("--sample-m", type=float, default=25.0)
    parser.add_argument("--cues-json", type=Path,
                        help="Jev/TTS-reviewed cues; WAV files are encoded to IMA ADPCM")
    parser.add_argument("--analysis-json", type=Path,
                        help="write deterministic turn candidates and Jev-ready state")
    args = parser.parse_args()

    raw_points, waypoints = parse_gpx(args.gpx)
    sampled = resample(raw_points, args.sample_m)
    markers = build_markers(raw_points, waypoints)
    cues = load_cues_json(args.cues_json, raw_points) if args.cues_json else deterministic_cues(sampled, markers)

    if args.analysis_json:
        analysis = {
            "route": {
                "distance_m": round(raw_points[-1].route_m, 1),
                "raw_points": len(raw_points),
                "sampled_points": len(sampled),
            },
            "turn_candidates": turn_candidates(sampled),
            "jev_questions": {
                "worth_announcing": "Noul: should this candidate become a spoken roadbook cue?",
                "cue_type": "Choice: left/right/fork/cp/climb/descent/danger/supply/none",
                "priority": "Score 1..5: how important is this cue for safe route following?",
            },
        }
        args.analysis_json.write_text(json.dumps(analysis, ensure_ascii=False, indent=2), encoding="utf-8")

    route_id = args.route_id
    if route_id == 0:
        route_id = zlib.crc32(args.gpx.read_bytes()) & 0xFFFFFFFF

    package = compile_route(
        sampled,
        markers,
        cues,
        route_id=route_id,
        name=args.name or args.gpx.stem,
    )
    args.output.write_bytes(package)
    print(f"route={args.output}")
    print(f"distance_m={raw_points[-1].route_m:.1f} points={len(sampled)} markers={len(markers)} cues={len(cues)}")
    print(f"size={len(package)} whole_crc32={zlib.crc32(package) & 0xFFFFFFFF:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(command())
