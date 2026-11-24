#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh the vendored engine or build data packs for Pico eSpeak."""

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[1]
UPSTREAM_REPO = "https://github.com/espeak-ng/espeak-ng.git"
UPSTREAM_REF = "ee9fa7cc4704d683020526f47a9655f14c8d5f4c"
SONIC_REPO = "https://github.com/waywardgeek/sonic.git"
SONIC_REF = "fbf75c3d6d846bad3bb3d456cbc5d07d9fd8c104"
CORE_SOURCES = """
    common mnemonics error ieee80 compiledata compiledict dictionary encoding
    intonation langopts numbers phoneme phonemelist readclause setlengths
    soundicon spect ssml synthdata synthesize tr_languages translate
    translateword voices wavegen speech espeak_api klatt sPlayer
""".split()
PUBLIC_HEADERS = ("encoding.h", "espeak_ng.h", "speak_lib.h")
BASE_FILES = ("phontab", "phonindex", "phondata", "intonations")
PACK_MAGIC = b"PESPACK1"
PACK_HEADER = struct.Struct("<8sIII20s")
PACK_ENTRY = struct.Struct("<III")
PACK_LIMIT = (1 << 31) - 1
PACK_MAX_PATH = 119
PACK_MAX_FILES = 1024
PRESETS = {
    "minimal-en": ("en", "none"),
    "english": ("en", "all"),
    "western-europe-lite": ("en,fr,de,es,it,pt,nl,sv,da,no,fi", "none"),
    "all": ("all", "all"),
}


def run(command, cwd=None):
    subprocess.run(command, cwd=cwd, check=True)


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def checkout(path, remote, ref, offline=False):
    if not path.exists():
        if offline:
            raise ValueError(f"No cached source checkout at {path}")
        path.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--quiet", remote, str(path)])
    if git(path, "remote", "get-url", "origin") != remote:
        raise ValueError(f"Unexpected origin in {path}")
    if git(path, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError(f"Commit or move local changes in {path} before continuing")
    pinned = re.fullmatch(r"[0-9a-f]{40}", ref) and subprocess.run(
        ["git", "cat-file", "-e", f"{ref}^{{commit}}"], cwd=path, stderr=subprocess.DEVNULL,
    ).returncode == 0
    if pinned and git(path, "rev-parse", "HEAD") == ref:
        return ref
    if not pinned and not offline:
        run(["git", "fetch", "--quiet", "--tags", "origin"], cwd=path)
    branch = f"refs/remotes/origin/{ref}"
    if subprocess.run(["git", "show-ref", "--verify", "--quiet", branch], cwd=path).returncode == 0:
        ref = branch
    if offline and subprocess.run(
        ["git", "cat-file", "-e", f"{ref}^{{commit}}"], cwd=path, stderr=subprocess.DEVNULL,
    ).returncode != 0:
        raise ValueError(f"Revision {ref} is unavailable in the cached checkout at {path}")
    run(["git", "checkout", "--quiet", "--detach", ref], cwd=path)
    return git(path, "rev-parse", "HEAD")


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def vendor_engine(upstream, sonic, revision):
    version = re.search(r"\bVERSION\s+(\d+\.\d+\.\d+)", (upstream / "CMakeLists.txt").read_text()).group(1)
    with TemporaryDirectory(prefix="pico-espeak-vendor-") as temporary:
        staging = Path(temporary)
        core = staging / "src/libespeak-ng"
        for name in CORE_SOURCES:
            copy(upstream / "src/libespeak-ng" / f"{name}.c", core / f"{name}.c")
        for source in (upstream / "src/libespeak-ng").glob("*.h"):
            copy(source, core / source.name)
        for folder in ("include", "src"):
            for source in (upstream / "src/speechPlayer" / folder).iterdir():
                if source.suffix in {".cpp", ".h"}:
                    copy(source, staging / "src/speechPlayer" / folder / source.name)
        copy(sonic / "sonic.c", core / "sonic.c")
        copy(sonic / "sonic.h", core / "sonic.h")
        run(["git", "apply", str(ROOT / "extras/espeak-ng.patch")], cwd=staging)
        (core / "config.h").write_text(f'''#pragma once

#define HAVE_MKSTEMP 0
#define HAVE_NBTOOL_CONFIG_H 0
#define HAVE_DECL_OPTRESET 0
#define HAVE_SYS_ENDIAN_H 0
#define HAVE_ISWBLANK 1

#define USE_ASYNC 0
#define USE_KLATT 1
#define USE_LIBPCAUDIO 0
#define USE_LIBSONIC 1
#define USE_MBROLA 0
#define USE_SPEECHPLAYER 1

#define PACKAGE_VERSION "{version}"
#define ESPEAK_ARDUINO_UPSTREAM_COMMIT "{revision}"

// Avoid a link conflict with sketches that declare their own global voice.
#define voice pico_espeak_engine_voice

#ifndef PICO_ESPEAK_MAPPED_DATA
#  if defined(ARDUINO)
#    define PICO_ESPEAK_MAPPED_DATA 1
#  else
#    define PICO_ESPEAK_MAPPED_DATA 0
#  endif
#endif

#if PICO_ESPEAK_MAPPED_DATA
#  define N_PATH_BUF 256
#endif
''')
        destination = ROOT / "src/espeak-ng"
        shutil.rmtree(destination / "core", ignore_errors=True)
        shutil.copytree(core, destination / "core")
        shutil.rmtree(destination / "speechPlayer", ignore_errors=True)
        shutil.copytree(staging / "src/speechPlayer", destination / "speechPlayer")
        for name in PUBLIC_HEADERS:
            copy(upstream / "src/include/espeak-ng" / name, destination / name)
        ucd = upstream / "src/ucd-tools/src"
        for source in ucd.glob("*.c"):
            copy(source, ROOT / "src/ucd" / source.name)
        copy(ucd / "include/ucd/ucd.h", ROOT / "src/ucd/ucd.h")
        for name in ("COPYING.APACHE", "COPYING.BSD2", "COPYING.UCD"):
            copy(upstream / name, ROOT / name)
        copy(upstream / "COPYING", ROOT / "LICENSE")
        copy(sonic / "LICENSE", ROOT / "COPYING.SONIC")
    print(f"Vendored eSpeak NG {version} at {revision}")


def build_data(upstream, build_dir, compact=False, offline=False):
    cmake = shutil.which("cmake")
    if cmake is None:
        raise ValueError("CMake is required to build speech data from source")
    extras = sorted((upstream / "dictsource/extra").glob("*_listx"))
    lexicons = [f"-DEXTRA_{source.name[:-6]}={'OFF' if compact else 'ON'}" for source in extras]
    disconnected = "ON" if offline else "OFF"
    run([cmake, "-S", str(upstream), "-B", str(build_dir),
         "-DBUILD_SHARED_LIBS=OFF", "-DENABLE_TESTS=OFF", "-DUSE_MBROLA=OFF",
         "-DUSE_LIBPCAUDIO=OFF", "-DUSE_LIBSONIC=OFF", "-DUSE_SPEECHPLAYER=OFF",
         "-DUSE_ASYNC=OFF", *lexicons, f"-DFETCHCONTENT_FULLY_DISCONNECTED={disconnected}"])
    run([cmake, "--build", str(build_dir), "--target", "data", "--parallel", "4"])
    return build_dir / "espeak-ng-data"


def parse_selection(value):
    words = {item.strip() for item in value.split(",") if item.strip()}
    if {"all", "none"}.intersection(words) and len(words) > 1:
        raise ValueError("Use 'all' or 'none' on its own")
    return words


@dataclass(frozen=True)
class VoiceFile:
    path: Path
    identifier: str
    name: str
    tags: tuple
    dictionary: str = ""


def voice_files(directory, variants=False):
    folder = directory / ("voices/!v" if variants else "lang")
    entries = sorted(path for path in folder.rglob("*") if path.is_file())
    result = []
    for path in entries:
        content = path.read_text(encoding="utf-8")
        name = re.search(r"^[ \t]*name[ \t]+([^\r\n]+)", content, re.MULTILINE)
        tags = re.findall(r"^[ \t]*language[ \t]+(\S+)", content, re.MULTILINE)
        dictionary = re.search(r"^[ \t]*dictionary[ \t]+(\S+)", content, re.MULTILINE)
        result.append(VoiceFile(
            path,
            path.relative_to(folder).as_posix(),
            name.group(1).strip() if name else path.name,
            tuple(tag.lower() for tag in tags),
            dictionary.group(1) if dictionary else "",
        ))
    return result


def select_voice_files(selection, files, keys, kind):
    if selection == {"all"}:
        return files
    if selection == {"none"}:
        return []
    unknown = selection.difference(key for voice in files for key in keys(voice))
    if unknown:
        raise ValueError(f"Unknown {kind} ({', '.join(sorted(unknown))}). See --list "
                         f"{'voices' if kind == 'variant' else 'languages'}.")
    return [voice for voice in files if selection.intersection(keys(voice))]


def exact_language_keys(voice):
    return {voice.identifier.lower(), voice.path.name.lower(), *voice.tags[:1]}


def broad_language_keys(voice):
    return exact_language_keys(voice).union(
        voice.tags, {voice.path.name.split("-")[0].lower()},
    )


def choose_data(directory, languages, variants, dialects=()):
    languages = {language.lower() for language in languages}
    dialects = {dialect.lower() for dialect in dialects}
    known_languages = voice_files(directory)
    broad = select_voice_files(languages, known_languages, broad_language_keys, "language")
    exact = select_voice_files(dialects, known_languages, exact_language_keys, "dialect")
    selected = {voice.path for voice in broad + exact}
    langs = [voice for voice in known_languages if voice.path in selected]
    if not langs:
        raise ValueError("Select at least one language or dialect")
    voices = select_voice_files(variants, voice_files(directory, variants=True),
                                lambda voice: {voice.identifier}, "variant")
    dictionaries = set()
    for voice in langs:
        if voice.dictionary:
            dictionaries.add(voice.dictionary)
        elif voice.tags:
            name = voice.tags[0].split("-")[0]
            if (directory / f"{name}_dict").is_file():
                dictionaries.add(name)
    if languages == {"all"}:
        dictionaries = {path.name[:-5] for path in directory.glob("*_dict")}

    files = {name: (directory / name).read_bytes() for name in BASE_FILES}
    if (directory / "config").is_file():
        files["config"] = (directory / "config").read_bytes()
    for name in sorted(dictionaries):
        filename = f"{name}_dict"
        files[filename] = (directory / filename).read_bytes()
    for folder, sources in (("lang", langs), ("voices", voices)):
        entries = []
        for voice in sources:
            relative = voice.path.relative_to(directory / folder).as_posix()
            files[f"{folder}/{relative}"] = voice.path.read_bytes()
            entries.append(relative)
        if entries:
            files[f"{folder}/manifest"] = ("\n".join(entries) + "\n").encode()
    return files


def validate_paths(names):
    seen = set()
    for name in names:
        if not isinstance(name, str) or name == "manifest.json" or "\\" in name or "\0" in name:
            raise ValueError(f"Invalid file path in data pack: {name!r}")
        if any(part in {"", ".", ".."} for part in name.split("/")):
            raise ValueError(f"Invalid file path in data pack: {name!r}")
        if name in seen:
            raise ValueError(f"Duplicate file in data pack: {name}")
        seen.add(name)
    for name in sorted(seen):
        for parent in PurePosixPath(name).parents:
            if parent.as_posix() in seen:
                raise ValueError(f"File and directory conflict in data pack: {parent}")


def pack_path(directory, name):
    path = directory / name
    if path.resolve() != path:
        raise ValueError(f"Data pack file or directory is a symbolic link: {name}")
    return path


def read_manifest(path):
    metadata = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(metadata, dict) or not isinstance(metadata.get("files"), list):
        raise ValueError(f"Invalid data pack manifest: {path}")
    validate_paths(metadata["files"])
    return metadata


def read_pack(directory, revision):
    directory = directory.resolve()
    metadata = read_manifest(pack_path(directory, "manifest.json"))
    if metadata.get("upstream_commit") != revision:
        raise ValueError("The source pack was built for a different eSpeak NG revision")
    integrity = metadata.get("sha256")
    if "sha256" in metadata and (
        not isinstance(integrity, dict) or set(integrity) != set(metadata["files"])
    ):
        raise ValueError("Missing or invalid SHA-256 entries in the data pack manifest")
    files = {}
    for name in metadata["files"]:
        contents = pack_path(directory, name).read_bytes()
        if integrity is not None and hashlib.sha256(contents).hexdigest() != integrity[name]:
            raise ValueError(f"SHA-256 mismatch in data pack: {name}")
        files[name] = contents
    if any(not files.get(name) for name in BASE_FILES):
        raise ValueError("The source pack is missing required phoneme files")
    if "total_bytes" in metadata and metadata["total_bytes"] != sum(map(len, files.values())):
        raise ValueError("Data pack size differs from its manifest")
    return files


def write_filesystem(directory, files, revision):
    if directory.name != "espeak-ng-data":
        raise ValueError("Name the filesystem output directory espeak-ng-data")
    directory = directory.resolve()
    validate_paths(files)
    destinations = {name: pack_path(directory, name) for name in files}
    old_manifest = pack_path(directory, "manifest.json")
    if old_manifest.exists():
        old_files = read_manifest(old_manifest)["files"]
        stale_files = [pack_path(directory, name) for name in set(old_files).difference(files)]
        for path in stale_files:
            path.unlink(missing_ok=True)
    for name, destination in destinations.items():
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(files[name])
    old_manifest.write_text(json.dumps({
        "upstream_commit": revision,
        "total_bytes": sum(len(data) for data in files.values()),
        "files": sorted(files),
        "sha256": {name: hashlib.sha256(files[name]).hexdigest() for name in sorted(files)},
    }, indent=2) + "\n")


def make_binary_pack(files, revision):
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("A binary voice pack requires a full eSpeak NG commit SHA")
    validate_paths(files)
    if not 0 < len(files) <= PACK_MAX_FILES:
        raise ValueError(f"A binary voice pack needs between 1 and {PACK_MAX_FILES} files")
    entries = []
    for name, data in sorted(files.items()):
        if len(name) > PACK_MAX_PATH or any(not " " <= character <= "~" for character in name):
            raise ValueError(f"Binary voice pack paths must be printable ASCII and at most "
                             f"{PACK_MAX_PATH} bytes: {name!r}")
        entries.append((name.encode("ascii") + b"\0", data))

    path_start = PACK_HEADER.size + PACK_ENTRY.size * len(entries)
    data_start = (path_start + sum(len(name) for name, _ in entries) + 3) & ~3
    total = data_start + sum((len(data) + 3) & ~3 for _, data in entries)
    if total > PACK_LIMIT:
        raise ValueError("Binary voice pack exceeds the 2 GB format limit")

    result = bytearray(total)
    PACK_HEADER.pack_into(result, 0, PACK_MAGIC, total, 0, len(entries), bytes.fromhex(revision))
    for index, (name, data) in enumerate(entries):
        PACK_ENTRY.pack_into(result, PACK_HEADER.size + PACK_ENTRY.size * index, path_start, data_start, len(data))
        result[path_start:path_start + len(name)] = name
        result[data_start:data_start + len(data)] = data
        path_start += len(name)
        data_start += (len(data) + 3) & ~3
    struct.pack_into("<I", result, 12, zlib.crc32(memoryview(result)[16:]))
    return result


def write_embedded(source, header, include, function, files, revision):
    if not re.fullmatch(r"[a-zA-Z_]\w*(?:::[a-zA-Z_]\w*)*", function, re.ASCII):
        raise ValueError("Give a C++ function name with an optional namespace")
    namespace, _, name = function.rpartition("::")
    declaration = f"const espeak::DataPack &{name}()"
    if header:
        if source.resolve() == header.resolve():
            raise ValueError("Choose different paths for the C++ source and header")
        header.parent.mkdir(parents=True, exist_ok=True)
        decl = f"{declaration};\n"
        if namespace:
            decl = f"namespace {namespace} {{\n{decl}}}\n"
        header.write_text(f"#pragma once\n\n#include <ESpeakData.h>\n\n{decl}")
        include = header.name

    source.parent.mkdir(parents=True, exist_ok=True)
    entries = sorted(files.items())
    with source.open("w", encoding="utf-8", newline="\n") as out:
        out.write(f"// Speech data from eSpeak NG {revision}. GPL-3.0-or-later.\n")
        out.write(f'#include "{include}"\n\nnamespace {{\n')
        for index, (_, data) in enumerate(entries):
            out.write(f"alignas(4) const uint8_t file{index}[] = {{\n")
            for offset in range(0, len(data), 16):
                out.write("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",\n")
            out.write("};\n\n")
        out.write("const espeak::DataFile files[] = {\n")
        for index, (path, _) in enumerate(entries):
            out.write(f"    {{{json.dumps(path, ensure_ascii=False)}, file{index}, sizeof(file{index})}},\n")
        out.write("};\n\nconst espeak::DataPack pack{files, sizeof(files) / sizeof(files[0])};\n}\n\n")
        if namespace:
            out.write(f"namespace {namespace} {{\n")
        out.write(f"{declaration}\n{{\n    return pack;\n}}\n")
        if namespace:
            out.write("}\n")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", default=UPSTREAM_REF, help="Upstream revision to use (default: the vendored commit)")
    parser.add_argument("--checkout", type=Path, default=ROOT / ".upstream/espeak-ng")
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".build/espeak-ng")
    parser.add_argument("--offline", action="store_true", help="Use cached source revisions without fetching")
    parser.add_argument("--update-core", action="store_true", help="Refresh the vendored C code and licenses")
    parser.add_argument("--skip-data", action="store_true", help="Only refresh the vendored engine")
    parser.add_argument("--preset", choices=PRESETS, help="Choose a language/variant set (default: english)")
    parser.add_argument("--list", choices=("languages", "voices", "presets"), help="List available voice pack selections")
    parser.add_argument("--languages", help="Comma-separated language codes or 'all'")
    parser.add_argument("--dialects", help="Comma-separated exact language voice IDs, such as en-US or pt-BR")
    parser.add_argument("--voices", help="Comma-separated variant filenames, 'all', or 'none'")
    parser.add_argument("--compact", action="store_true", help="Omit upstream's optional extra dictionary entries")
    parser.add_argument("--max-data-bytes", type=int,
                        help="Limit payload bytes, or total file bytes for binary packs")
    parser.add_argument("--format", choices=("filesystem", "embedded", "binary"), default="embedded")
    parser.add_argument("--from-data", type=Path, help="Reuse a generated filesystem pack with a manifest.json")
    parser.add_argument("--data-out", type=Path, help="Filesystem output directory or binary voice pack filename")
    parser.add_argument("--embedded-source", type=Path, default=Path("ESpeakDataPack.cpp"))
    parser.add_argument("--embedded-header", type=Path)
    parser.add_argument("--include", help="Existing declaration header to include instead of generating one")
    parser.add_argument("--pack-function", default="speechDataPack")
    args = parser.parse_args(argv)

    if args.skip_data and not args.update_core:
        parser.error("--skip-data requires --update-core")
    if args.format != "embedded" and not (args.skip_data or args.list or args.data_out):
        parser.error(f"--format {args.format} requires --data-out")
    if args.include and args.embedded_header:
        parser.error("Use either --include or --embedded-header")
    if args.max_data_bytes is not None and args.max_data_bytes <= 0:
        parser.error("--max-data-bytes must be positive")
    if args.list and (args.update_core or args.from_data):
        parser.error("--list uses the pinned upstream source and is independent of other output")
    if args.from_data and any((args.preset, args.languages, args.dialects, args.voices, args.compact)):
        parser.error("--from-data reuses the existing pack; selection flags require source data")

    if args.list == "presets":
        for name, (languages, voices) in PRESETS.items():
            print(f"{name}\tlanguages={languages}\tvoices={voices}")
        return

    revision = args.ref
    if args.update_core or args.from_data is None:
        upstream = args.checkout.resolve()
        revision = checkout(upstream, UPSTREAM_REPO, args.ref, offline=args.offline)
        if args.list:
            variants = args.list == "voices"
            for voice in voice_files(upstream / "espeak-ng-data", variants):
                print(f"{voice.identifier}\t{voice.name}" + ("" if variants else f"\t{', '.join(voice.tags)}"))
            return
        if args.update_core:
            sonic = ROOT / ".upstream/sonic"
            checkout(sonic, SONIC_REPO, SONIC_REF, offline=args.offline)
            vendor_engine(upstream, sonic, revision)
        if args.skip_data:
            return
    if args.from_data:
        files = read_pack(args.from_data.resolve(), revision)
    else:
        build_name = revision + ("-compact" if args.compact else "")
        data = build_data(upstream, args.build_dir.resolve() / build_name,
                          compact=args.compact, offline=args.offline)
        languages, voices = PRESETS[args.preset or "english"]
        if args.dialects and args.preset is None:
            languages = ""
        files = choose_data(data, parse_selection(args.languages or languages),
                            parse_selection(args.voices or voices), parse_selection(args.dialects or ""))
    payload_size = sum(map(len, files.values()))
    binary = make_binary_pack(files, revision) if args.format == "binary" else None
    output_size = len(binary) if binary is not None else payload_size
    if args.max_data_bytes is not None and output_size > args.max_data_bytes:
        name = "Binary voice pack" if binary is not None else "Pack data"
        raise ValueError(f"{name} needs {output_size:,} bytes, exceeding the {args.max_data_bytes:,}-byte limit")
    if args.format == "filesystem":
        write_filesystem(args.data_out.resolve(), files, revision)
    elif args.format == "binary":
        args.data_out.parent.mkdir(parents=True, exist_ok=True)
        args.data_out.write_bytes(binary)
        print(f"Wrote {len(binary):,} bytes to {args.data_out}")
    else:
        header = None if args.include else args.embedded_header or args.embedded_source.with_suffix(".h")
        write_embedded(args.embedded_source, header, args.include, args.pack_function, files, revision)
    print(f"Packed {len(files)} files ({payload_size:,} data bytes)")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
