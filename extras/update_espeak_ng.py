#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh the vendored engine or build data packs for Pico eSpeak."""

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
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


def checkout(path, remote, ref):
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--quiet", remote, str(path)])
    if git(path, "remote", "get-url", "origin") != remote:
        raise ValueError(f"Unexpected origin in {path}")
    if git(path, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError(f"Commit or move local changes in {path} before continuing")
    run(["git", "fetch", "--quiet", "--tags", "origin"], cwd=path)
    branch = f"refs/remotes/origin/{ref}"
    if subprocess.run(["git", "show-ref", "--verify", "--quiet", branch], cwd=path).returncode == 0:
        ref = branch
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


def build_data(upstream, build_dir):
    cmake = shutil.which("cmake")
    if cmake is None:
        raise ValueError("CMake is required to build speech data from source")
    run([cmake, "-S", str(upstream), "-B", str(build_dir),
         "-DBUILD_SHARED_LIBS=OFF", "-DENABLE_TESTS=OFF", "-DUSE_MBROLA=OFF",
         "-DUSE_LIBPCAUDIO=OFF", "-DUSE_LIBSONIC=OFF", "-DUSE_SPEECHPLAYER=OFF",
         "-DUSE_ASYNC=OFF"])
    run([cmake, "--build", str(build_dir), "--target", "data", "--parallel", "4"])
    return build_dir / "espeak-ng-data"


def parse_selection(value):
    return {item.strip() for item in value.split(",") if item.strip()}


def choose_data(directory, languages, variants):
    known_languages = sorted(path for path in (directory / "lang").rglob("*") if path.is_file())
    known_variants = sorted(path for path in (directory / "voices/!v").glob("*") if path.is_file())
    language_text = {path: path.read_text(encoding="utf-8") for path in known_languages}
    language_codes = {
        path: re.findall(r"^\s*language\s+(\S+)", content, re.MULTILINE)
        for path, content in language_text.items()
    }

    def select(selection, files, keys):
        if selection == {"all"}:
            return files
        if selection == {"none"}:
            return []
        unknown = selection.difference(key for path in files for key in keys(path))
        if unknown:
            raise ValueError(f"Unknown language or voice: {', '.join(sorted(unknown))}")
        return [path for path in files if selection.intersection(keys(path))]

    def language_keys(path):
        keys = {path.name.lower(), path.name.split("-")[0].lower()}
        return keys.union(code.lower() for code in language_codes[path])

    langs = select({language.lower() for language in languages}, known_languages, language_keys)
    voices = select(variants, known_variants, lambda p: {p.name})
    dictionaries = set()
    for path in langs:
        explicit = re.search(r"^\s*dictionary\s+(\S+)", language_text[path], re.MULTILINE)
        if explicit:
            dictionaries.add(explicit.group(1))
        elif language_codes[path]:
            name = language_codes[path][0].split("-")[0].lower()
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
        for path in sources:
            relative = path.relative_to(directory / folder).as_posix()
            files[f"{folder}/{relative}"] = path.read_bytes()
            entries.append(relative)
        if entries:
            files[f"{folder}/manifest"] = ("\n".join(entries) + "\n").encode()
    return files


def read_pack(directory, revision):
    metadata = json.loads((directory / "manifest.json").read_text())
    if metadata["upstream_commit"] != revision:
        raise ValueError("The source pack was built for a different eSpeak NG revision")
    files = {}
    for name in metadata["files"]:
        path = (directory / name).resolve()
        if not path.is_relative_to(directory) or path == directory:
            raise ValueError(f"Invalid file path in data pack: {name}")
        files[name] = path.read_bytes()
    if any(not files.get(name) for name in BASE_FILES):
        raise ValueError("The source pack is missing required phoneme files")
    return files


def write_filesystem(directory, files, revision):
    if directory.name != "espeak-ng-data":
        raise ValueError("Name the filesystem output directory espeak-ng-data")
    old_manifest = directory / "manifest.json"
    if old_manifest.exists():
        old_files = json.loads(old_manifest.read_text())["files"]
        for name in set(old_files).difference(files):
            path = directory / name
            if path.resolve().is_relative_to(directory) and path.is_file():
                path.unlink()
    for name, data in files.items():
        destination = directory / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    old_manifest.write_text(json.dumps({
        "upstream_commit": revision,
        "total_bytes": sum(len(data) for data in files.values()),
        "files": sorted(files),
    }, indent=2) + "\n")


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
    parser.add_argument("--update-core", action="store_true", help="Refresh the vendored C code and licenses")
    parser.add_argument("--skip-data", action="store_true", help="Only refresh the vendored engine")
    parser.add_argument("--preset", choices=PRESETS, default="english")
    parser.add_argument("--languages", help="Comma-separated language codes or 'all'")
    parser.add_argument("--voices", help="Comma-separated variant filenames, 'all', or 'none'")
    parser.add_argument("--format", choices=("filesystem", "embedded"), default="embedded")
    parser.add_argument("--from-data", type=Path, help="Reuse a generated filesystem pack with a manifest.json")
    parser.add_argument("--data-out", type=Path, help="Filesystem output directory, named espeak-ng-data")
    parser.add_argument("--embedded-source", type=Path, default=Path("ESpeakDataPack.cpp"))
    parser.add_argument("--embedded-header", type=Path)
    parser.add_argument("--include", help="Existing declaration header to include instead of generating one")
    parser.add_argument("--pack-function", default="speechDataPack")
    args = parser.parse_args(argv)

    if args.skip_data and not args.update_core:
        parser.error("--skip-data requires --update-core")
    if args.format == "filesystem" and not args.skip_data and args.data_out is None:
        parser.error("--format filesystem requires --data-out")
    if args.include and args.embedded_header:
        parser.error("Use either --include or --embedded-header")

    revision = args.ref
    if args.update_core or args.from_data is None:
        upstream = args.checkout.resolve()
        revision = checkout(upstream, UPSTREAM_REPO, args.ref)
        if args.update_core:
            sonic = ROOT / ".upstream/sonic"
            checkout(sonic, SONIC_REPO, SONIC_REF)
            vendor_engine(upstream, sonic, revision)
        if args.skip_data:
            return
    if args.from_data:
        files = read_pack(args.from_data.resolve(), revision)
    else:
        data = build_data(upstream, args.build_dir.resolve() / revision)
        languages, voices = PRESETS[args.preset]
        files = choose_data(data, parse_selection(args.languages or languages), parse_selection(args.voices or voices))
    if args.format == "filesystem":
        write_filesystem(args.data_out.resolve(), files, revision)
    else:
        header = None if args.include else args.embedded_header or args.embedded_source.with_suffix(".h")
        write_embedded(args.embedded_source, header, args.include, args.pack_function, files, revision)
    print(f"Packed {len(files)} files ({sum(len(data) for data in files.values()):,} bytes)")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
