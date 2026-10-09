from __future__ import annotations

import importlib.util
import json
import sys
import struct
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("route_compiler", ROOT / "tools" / "route_compiler.py")
assert SPEC and SPEC.loader
rc = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = rc
SPEC.loader.exec_module(rc)


class RouteCompilerTests(unittest.TestCase):
    def _write_gpx(self, path: Path) -> None:
        path.write_text(
            """<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="test" xmlns="http://www.topografix.com/GPX/1/1">
  <wpt lat="35.00045" lon="139.00045"><name>CP1</name></wpt>
  <trk><name>Route</name><trkseg>
    <trkpt lat="35.00000" lon="139.00000"><ele>10</ele></trkpt>
    <trkpt lat="35.00030" lon="139.00000"><ele>12</ele></trkpt>
    <trkpt lat="35.00060" lon="139.00020"><ele>15</ele></trkpt>
    <trkpt lat="35.00060" lon="139.00060"><ele>16</ele></trkpt>
    <trkpt lat="35.00090" lon="139.00060"><ele>20</ele></trkpt>
  </trkseg></trk>
</gpx>
""",
            encoding="utf-8",
        )

    def test_compile_route_has_valid_crc_and_sections(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gpx = root / "route.gpx"
            self._write_gpx(gpx)
            points, waypoints = rc.parse_gpx(gpx)
            sampled = rc.resample(points, 25.0)
            markers = rc.build_markers(points, waypoints)
            cues = rc.deterministic_cues(sampled, markers)
            package = rc.compile_route(sampled, markers, cues, route_id=77, name="CTRA TEST")

            header = rc.HEADER_STRUCT.unpack(package[: rc.HEADER_SIZE])
            self.assertEqual(header[0], b"FAPR")
            self.assertEqual(header[1], rc.VERSION)
            self.assertEqual(header[3], len(package))
            self.assertEqual(header[5], 77)
            self.assertGreaterEqual(header[7], 2)
            self.assertGreaterEqual(header[8], 3)
            self.assertEqual(zlib.crc32(package[rc.HEADER_SIZE :]) & 0xFFFFFFFF, header[4])

    def test_resample_interpolates_exact_spacing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            gpx = Path(tmp) / "route.gpx"
            self._write_gpx(gpx)
            points, _ = rc.parse_gpx(gpx)
            sampled = rc.resample(points, 25.0)
            self.assertGreater(len(sampled), 3)
            for index, point in enumerate(sampled[:-1]):
                self.assertAlmostEqual(point.route_m, index * 25.0, places=5)
            self.assertAlmostEqual(sampled[-1].route_m, points[-1].route_m, places=5)

    def test_analysis_candidates_are_json_serializable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            gpx = Path(tmp) / "route.gpx"
            self._write_gpx(gpx)
            points, _ = rc.parse_gpx(gpx)
            sampled = rc.resample(points, 10.0)
            json.dumps(rc.turn_candidates(sampled))

    def test_wav_is_encoded_to_custom_ima_stream(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = Path(tmp) / "cue.wav"
            samples = [0, 1000, -1000, 2000, -2000, 0] * 40
            with wave.open(str(wav_path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(8000)
                wav.writeframes(struct.pack("<" + "h" * len(samples), *samples))
            pcm, rate = rc.read_pcm16_wav(wav_path)
            encoded = rc.encode_ima_adpcm(pcm)
            self.assertEqual(rate, 8000)
            self.assertGreater(len(encoded), 4)
            predictor, index, reserved = struct.unpack("<hBB", encoded[:4])
            self.assertEqual(predictor, samples[0])
            self.assertEqual(index, 0)
            self.assertEqual(reserved, 0)

    def test_off_route_audio_cue_can_be_global_without_route_m(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gpx = root / "route.gpx"
            self._write_gpx(gpx)
            wav_path = root / "off-route.wav"
            samples = [0, 1200, -1200, 0] * 80
            with wave.open(str(wav_path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(8000)
                wav.writeframes(struct.pack("<" + "h" * len(samples), *samples))

            cues_path = root / "cues.json"
            cues_path.write_text(
                json.dumps(
                    {
                        "cues": [
                            {
                                "type": "off_route",
                                "priority": 5,
                                "audio_wav": wav_path.name,
                            },
                            {
                                "route_m": 80,
                                "type": "right",
                                "trigger_before_m": 40,
                                "priority": 3,
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )

            points, _ = rc.parse_gpx(gpx)
            cues = rc.load_cues_json(cues_path, points)
            self.assertEqual(cues[0].cue_type, rc.CUE_TYPES["off_route"])
            self.assertEqual(cues[0].route_m, 0)
            self.assertGreater(len(cues[0].audio), 4)
            self.assertEqual(cues[0].sample_rate, 8000)


if __name__ == "__main__":
    unittest.main()
