#!/usr/bin/env python3
"""Build the optional offline RHVoice/SLT ARM runtime from pinned source."""
import argparse
import json
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REVISIONS = {
    "engine": "fa9dd196fd2dac3b0bf089a2d80fc8477c2380e3",
    "english": "8a2ea34df72a190dae245460aeb20a867d64775c",
    "slt": "c17dded68322016aba2868d9c29f11bba14d275f",
    "bdl": "ea6b944550af6f9b33f2cc5e20f38d3b19087068",
    "clb": "e0aa06cf50334f10844d1a28a72b3fdcf3cdd150",
    "alan": "bb3fdb15b9c64109f396f56f6155965bfcf89977",
}


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def fetch(repo, revision, destination):
    if not (destination / ".git").exists():
        destination.mkdir(parents=True, exist_ok=True)
        run("git", "init", "-q", destination)
        run("git", "-C", destination, "remote", "add", "origin", "https://github.com/" + repo + ".git")
        run("git", "-C", destination, "fetch", "-q", "--depth", "1", "origin", revision)
        run("git", "-C", destination, "checkout", "-q", "--detach", "FETCH_HEAD")
    actual = subprocess.check_output(["git", "-C", str(destination), "rev-parse", "HEAD"], text=True).strip()
    if actual != revision:
        raise ValueError("Unexpected source revision in " + str(destination))


def copy_tree(source, target):
    shutil.copytree(source, target, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".git"))


def build(source, output, cc, cxx, cmake, jobs):
    fetch("RHVoice/RHVoice", REVISIONS["engine"], source)
    fetch("RHVoice/English", REVISIONS["english"], source / "data/languages/English")
    for name in ("slt","bdl","clb","alan"):
        fetch("RHVoice/"+name+"-eng", REVISIONS[name], source / "data/voices" / name)
    # Only one Boost algorithm was used by the synthesis core. Replace that
    # expression with equivalent standard-library code to avoid a large
    # unrelated Boost dependency in this standalone static build.
    voice = source / "src/core/voice.cpp"
    original = 'id=boost::replace_all_copy(boost::to_lower_copy(name.get(), std::locale::classic()), "-", "_");'
    replacement = '''{
        id=name.get();
        std::transform(id.begin(), id.end(), id.begin(), [](char c) { return std::tolower(c, std::locale::classic()); });
        std::replace(id.begin(), id.end(), '-', '_');
      }'''
    text = voice.read_text()
    if original in text:
        text = text.replace('#include "boost/algorithm/string.hpp"', '#include <algorithm>')
        voice.write_text(text.replace(original, replacement))
    elif replacement not in text:
        raise ValueError("Unexpected RHVoice voice.cpp; patch no longer matches")
    build_dir = output.parent / (output.name + "-build")
    run(cmake, "-S", ROOT / "rhvoice", "-B", build_dir,
        "-DRHVOICE_SOURCE=" + str(source.resolve()), "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=arm",
        "-DCMAKE_C_COMPILER=" + cc, "-DCMAKE_CXX_COMPILER=" + cxx,
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY")
    run(cmake, "--build", build_dir, "--parallel", jobs)
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build_dir / "rhvoice-slt", output / "rhvoice-slt")
    (output / "rhvoice-slt").chmod(0o755)
    copy_tree(source / "data/languages/English", output / "data/languages/English")
    for name in ("slt","bdl","clb","alan"):
        copy_tree(source / "data/voices" / name, output / "data/voices" / name)
    config = output / "data/config"
    config.mkdir(parents=True, exist_ok=True)
    (config / "RHVoice.conf").write_text("# High-quality 24 kHz SLT; rate control uses Sonic.\nquality=max\n")
    shutil.copyfile(source / "licenses/gpl-3.0.txt", output / "LICENSE-GPL-3.0")
    (output / "SOURCES.json").write_text(json.dumps(REVISIONS, indent=2) + "\n")
    # Include corresponding source alongside the binary. The archive contains
    # the exact patched core, data, adapter, Sonic and offline build recipe.
    with tarfile.open(output / "corresponding-source.tar.gz", "w:gz") as archive:
        def keep(info):
            return None if ".git" in Path(info.name).parts else info
        for relative in ("src", "licenses", "LICENSE.md", "README.md", "data/languages/English", "data/voices/slt", "data/voices/bdl", "data/voices/clb", "data/voices/alan"):
            archive.add(source / relative, "source/RHVoice/" + relative, filter=keep)
        archive.add(ROOT / "rhvoice", "source/era/rhvoice")
        for name in ("sonic.c", "sonic.h", "LICENSE"):
            archive.add(ROOT / "tempo" / name, "source/era/tempo/" + name)
    print("RHVoice/SLT runtime: " + str(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=ROOT / ".cache/rhvoice-source")
    parser.add_argument("--output", type=Path, default=ROOT / ".cache/rhvoice-runtime")
    parser.add_argument("--cc", default="arm-linux-gnueabihf-gcc")
    parser.add_argument("--cxx", default="arm-linux-gnueabihf-g++")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    build(args.source_dir, args.output, args.cc, args.cxx, args.cmake, args.jobs)


if __name__ == "__main__":
    main()
