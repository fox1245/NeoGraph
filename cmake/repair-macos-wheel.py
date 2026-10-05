#!/usr/bin/env python3
"""Delocate a wheel, then sanitize every packaged Mach-O before writing RECORD.

Delocate 0.13's sanitize_rpaths follows non-system dependency edges, so a
copied library depending only on system libraries can retain build-host rpaths.
Run the final pass over payloads, not dependency edges or Homebrew originals.
"""

import argparse
import logging
from pathlib import Path
import shutil
import subprocess
import tempfile

from delocate import delocate_wheel
from delocate.tools import MACHO_MAGIC, ensure_writable, get_archs, replace_signature
from delocate.wheeltools import InWheelCtx
from macholib.MachO import MachO, lc_str_value
from macholib.mach_o import LC_RPATH
from packaging.version import Version


LOGGER = logging.getLogger(__name__)


def portable_rpath(value):
    return (value in ("@loader_path", "@executable_path")
            or value.startswith(("@loader_path/", "@executable_path/", "@rpath/",
                                 "/usr/lib/", "/System/Library/"))
            or value in ("/usr/lib", "/System/Library"))


def rpaths(header):
    return [lc_str_value(command[1].path, command).decode("utf-8")
            for command in header.commands if command[0].cmd == LC_RPATH]


def sanitize_slice(path):
    """Use install_name_tool on a thin payload, including duplicate LC_RPATHs."""
    (header,) = MachO(str(path)).headers
    original = rpaths(header)
    for value in original:
        if not portable_rpath(value):
            LOGGER.info("Deleting rpath %r from %s", value, path)
            # Like delocate's deletion pattern, delete duplicates separately.
            subprocess.run(["install_name_tool", "-delete_rpath", value, str(path)],
                           check=True)
    (header,) = MachO(str(path)).headers
    if rpaths(header) != [value for value in original if portable_rpath(value)]:
        raise RuntimeError("LC_RPATH sanitization did not preserve portable paths: " + str(path))


@ensure_writable
def sanitize_payload(filename):
    path = Path(filename)
    headers = MachO(filename).headers
    if all(portable_rpath(value) for header in headers for value in rpaths(header)):
        return
    if len(headers) == 1:
        sanitize_slice(path)
    else:
        # install_name_tool requires each deleted path to exist in EVERY slice.
        # Thin/recombine only the temporary wheel copy to support differing
        # per-architecture rpaths while retaining all original architectures.
        with tempfile.TemporaryDirectory(prefix="neograph-macho-") as temporary:
            slices = []
            for arch in sorted(get_archs(filename)):
                thin = Path(temporary) / arch
                subprocess.run(["lipo", filename, "-thin", arch, "-output", str(thin)],
                               check=True)
                sanitize_slice(thin)
                slices.append(str(thin))
            combined = Path(temporary) / "combined"
            subprocess.run(["lipo", "-create", *slices, "-output", str(combined)],
                           check=True)
            shutil.copyfile(combined, path)
    # install_name_tool (and lipo for fat payloads) invalidates the signature.
    # Use delocate's supported signing API, once after all load-command edits.
    replace_signature(filename, "-")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-archs", required=True)
    parser.add_argument("--require-target-macos-version", required=True, type=Version)
    parser.add_argument("-w", "--wheel-dir", required=True, type=Path)
    parser.add_argument("wheel", type=Path)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO)
    wheel = args.wheel.resolve(strict=True)
    destination = args.wheel_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    required_archs = (args.require_archs.split(",") if "," in args.require_archs
                      else args.require_archs)
    with tempfile.TemporaryDirectory(prefix="neograph-delocate-") as temporary:
        delocate_wheel(
            str(wheel), str(Path(temporary) / wheel.name),
            require_archs=required_archs,
            require_target_macos_version=args.require_target_macos_version,
            # Preserve portable/system rpaths, which upstream sanitization drops.
            # First resolve/copy/relink all dependencies; sanitize only afterwards.
            sanitize_rpaths=False,
        )
        # Delocate may rename the wheel to match its verified deployment tag.
        (repaired,) = Path(temporary).glob("*.whl")
        with InWheelCtx(str(repaired)) as context:
            for path in sorted(Path(context.wheel_path).rglob("*")):
                if not path.is_file():
                    continue
                with path.open("rb") as stream:
                    if stream.read(4) not in MACHO_MAGIC:
                        continue
                sanitize_payload(str(path))
            # Set output only after all payloads succeed. InWheelCtx regenerates
            # RECORD after signing, and retains delocate's WHEEL/platform tags.
            context.out_wheel = str(destination / repaired.name)
        LOGGER.info("Output: %s", destination / repaired.name)


if __name__ == "__main__":
    main()
