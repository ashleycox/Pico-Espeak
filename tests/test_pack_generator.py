# SPDX-License-Identifier: GPL-3.0-or-later
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from extras import update_espeak_ng as generator


class PackGeneratorTests(unittest.TestCase):
    def test_binary_layout(self):
        files = {"voices/!v/Mr serious": b"longer", "lang/gmw/en": b"abc", "unused": b""}
        pack = generator.make_binary_pack(files, generator.UPSTREAM_REF)
        magic, total, crc, count, revision = struct.unpack_from("<8sIII20s", pack)
        self.assertEqual(magic, b"PESPACK1")
        self.assertEqual(total, len(pack))
        self.assertEqual(crc, zlib.crc32(pack[16:]))
        self.assertEqual(count, len(files))
        self.assertEqual(revision.hex(), generator.UPSTREAM_REF)

        expected_path_offset = 40 + 12 * count
        expected_data_offset = (expected_path_offset + sum(len(path) + 1 for path in files) + 3) & ~3
        for index, (path, data) in enumerate(sorted(files.items())):
            path_offset, data_offset, size = struct.unpack_from("<III", pack, 40 + 12 * index)
            self.assertEqual(path_offset, expected_path_offset)
            self.assertEqual(data_offset, expected_data_offset)
            self.assertEqual(size, len(data))
            self.assertEqual(pack[path_offset:path_offset + len(path) + 1], path.encode() + b"\0")
            self.assertEqual(pack[data_offset:data_offset + size], data)
            expected_path_offset += len(path) + 1
            expected_data_offset += (size + 3) & ~3
            self.assertEqual(pack[data_offset + size:expected_data_offset], b"\0" * ((-size) % 4))
        self.assertEqual(expected_data_offset, len(pack))
        self.assertEqual(pack, generator.make_binary_pack(dict(reversed(list(files.items()))), generator.UPSTREAM_REF))

    def test_binary_rejects_unsafe_or_unsupported_paths(self):
        for name in ("", "/absolute", "voices/../other", "lang//en", "lang\\en", "lang/\nen",
                     "lang/./en", "lang/é", "a" * 120):
            with self.subTest(name=name), self.assertRaises(ValueError):
                generator.make_binary_pack({name: b"x"}, generator.UPSTREAM_REF)
        with self.assertRaisesRegex(ValueError, "File and directory conflict"):
            generator.make_binary_pack({"lang": b"x", "lang/en": b"y"}, generator.UPSTREAM_REF)
        with self.assertRaisesRegex(ValueError, "full eSpeak NG commit"):
            generator.make_binary_pack({"lang/en": b"x"}, "master")

    def test_filesystem_manifest_detects_tampering_and_accepts_legacy_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "espeak-ng-data"
            files = {name: name.encode() for name in generator.BASE_FILES}
            generator.write_filesystem(directory, files, generator.UPSTREAM_REF)
            self.assertEqual(generator.read_pack(directory, generator.UPSTREAM_REF), files)
            (directory / "phontab").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch.*phontab"):
                generator.read_pack(directory, generator.UPSTREAM_REF)
            (directory / "phontab").write_bytes(files["phontab"])
            manifest = directory / "manifest.json"
            old_metadata = json.loads(manifest.read_text())
            del old_metadata["sha256"]
            manifest.write_text(json.dumps(old_metadata))
            self.assertEqual(generator.read_pack(directory, generator.UPSTREAM_REF), files)
            old_metadata["files"].append("../outside")
            manifest.write_text(json.dumps(old_metadata))
            with self.assertRaisesRegex(ValueError, "Invalid file path"):
                generator.read_pack(directory, generator.UPSTREAM_REF)

    def test_binary_budget_includes_header_and_alignment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "espeak-ng-data"
            files = {name: b"x" for name in generator.BASE_FILES}
            generator.write_filesystem(directory, files, generator.UPSTREAM_REF)
            output = root / "voice-pack.bin"
            with self.assertRaisesRegex(ValueError, "Binary voice pack needs"):
                generator.main(["--from-data", str(directory), "--format", "binary",
                                "--data-out", str(output), "--max-data-bytes", "4"])
            self.assertFalse(output.exists())

    def test_language_family_and_exact_dialect_selection(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            files = {
                **{name: b"test" for name in generator.BASE_FILES},
                "en_dict": b"dictionary",
                "lang/gmw/en": b"name Great Britain\nlanguage en-gb\nlanguage en\n",
                "lang/gmw/en-US": b"name America\nlanguage en-us\nlanguage en\n",
                "lang/gmw/en-US-nyc": b"name New York City\nlanguage en-us-nyc\n",
                "voices/!v/f2": b"name female2\nlanguage variant\n",
            }
            for name, data in files.items():
                destination = directory / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            exact = generator.choose_data(directory, set(), {"f2"}, {"en-us"})
            self.assertEqual(exact["en_dict"], b"dictionary")
            self.assertEqual(exact["lang/manifest"], b"gmw/en-US\n")
            self.assertEqual(exact["voices/manifest"], b"!v/f2\n")
            self.assertNotIn("lang/gmw/en", exact)
            self.assertNotIn("lang/gmw/en-US-nyc", exact)
            family = generator.choose_data(directory, {"en"}, {"none"})
            self.assertEqual(family["lang/manifest"], b"gmw/en\ngmw/en-US\ngmw/en-US-nyc\n")
            with self.assertRaisesRegex(ValueError, "Unknown dialect.*--list languages"):
                generator.choose_data(directory, set(), {"none"}, {"missing"})


if __name__ == "__main__":
    unittest.main()
