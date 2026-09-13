#!/usr/bin/env python3
"""Builds the PiPL resource for AB Glow.

After Effects still reads plug-in metadata from a PiPL resource. Adobe ships
PiPLtool.exe for that, but it only runs on Windows and needs the MSVC
preprocessor, so this script writes the same binary directly. The layout
follows Examples/Resources/AE_General.r from the After Effects SDK.

Outputs:
  pipl.bin            raw PiPL resource data
  <name>_PiPL.rc      resource script referencing pipl.bin
  AbGlowPiPLFlags.h   the out-flags, shared with the C++ GlobalSetup
  <name>.r            cross-platform PiPL source, for macOS builds with Rez
"""

import argparse
import os
import re
import struct
import sys

# After Effects out-flags, mirrored from AE_Effect.h. The generated header lets
# the C++ side static_assert that these match the SDK enums.
PF_OutFlag_I_EXPAND_BUFFER = 1 << 9
PF_OutFlag_DEEP_COLOR_AWARE = 1 << 25

PF_OutFlag2_SUPPORTS_SMART_RENDER = 1 << 10
PF_OutFlag2_FLOAT_COLOR_AWARE = 1 << 12
PF_OutFlag2_SUPPORTS_THREADED_RENDERING = 1 << 27

OUT_FLAGS = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_I_EXPAND_BUFFER
OUT_FLAGS_2 = (PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE |
               PF_OutFlag2_SUPPORTS_THREADED_RENDERING)

# Windows on Arm entry points need the CodeWinARM64 PiPL key, which only exists
# in the AE 25.6 SDK and later. Set it from that SDK's AE_General.r to enable
# ARM64 builds.
WIN_ARM64_FOURCC = None

PF_STAGE_RELEASE = 3

ENTRY_POINT = "EffectMain"


def fourcc(code):
    """PiPL keys are stored byte reversed on Windows."""
    data = code.encode("ascii")
    if len(data) != 4:
        raise ValueError("four character code expected: %r" % code)
    return bytes(reversed(data))


def pack_version(major, minor, bug, stage, build):
    return ((major & 0x7) << 19 | (minor & 0xF) << 15 | (bug & 0xF) << 11 | (stage & 0x3) << 9 |
            (build & 0x1FF) | (major >> 3 & 0xF) << 26)


class PiplBuilder:
    def __init__(self):
        self.properties = []

    def _property(self, key, payload):
        self.properties.append(fourcc("8BIM") + fourcc(key) + struct.pack("<I", 0) +
                               struct.pack("<I", len(payload)) + payload)

    @staticmethod
    def _pad(data):
        remainder = len(data) % 4
        return data if remainder == 0 else data + b"\0" * (4 - remainder)

    def pstring(self, key, text):
        raw = text.encode("ascii")
        if len(raw) > 255:
            raise ValueError("string too long for a PiPL pstring: %r" % text)
        self._property(key, self._pad(bytes([len(raw)]) + raw))

    def cstring(self, key, text):
        self._property(key, self._pad(text.encode("ascii") + b"\0"))

    def literal(self, key, code):
        self._property(key, fourcc(code))

    def uint32(self, key, value):
        self._property(key, struct.pack("<I", value & 0xFFFFFFFF))

    def uint16_pair(self, key, first, second):
        self._property(key, struct.pack("<HH", first, second))

    def build(self):
        header = b"\x01\x00" + struct.pack("<I", 0) + struct.pack("<I", len(self.properties))
        return header + b"".join(self.properties)


def read_spec_version(sdk_root):
    """Reads PF_PLUG_IN_VERSION / SUBVERS from the SDK's AE_EffectVers.h."""
    path = os.path.join(sdk_root, "Headers", "AE_EffectVers.h")
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    except OSError as error:
        raise SystemExit("cannot read %s: %s" % (path, error))

    def find(name):
        match = re.search(r"#define\s+%s\s+(\d+)" % name, text)
        if match is None:
            raise SystemExit("%s not found in %s" % (name, path))
        return int(match.group(1))

    return find("PF_PLUG_IN_VERSION"), find("PF_PLUG_IN_SUBVERS")


def build_pipl(args, spec_version):
    builder = PiplBuilder()
    builder.literal("kind", "eFKT")
    builder.pstring("name", args.name)
    builder.pstring("catg", args.category)
    builder.cstring("8664", ENTRY_POINT)
    if WIN_ARM64_FOURCC:
        builder.cstring(WIN_ARM64_FOURCC, ENTRY_POINT)
    builder.uint16_pair("ePVR", 2, 0)
    builder.uint16_pair("eSVR", spec_version[0], spec_version[1])
    builder.uint32("eVER", pack_version(args.version_major, args.version_minor, args.version_bug,
                                        PF_STAGE_RELEASE, args.version_build))
    builder.uint32("eINF", 0)
    builder.uint32("eGLO", OUT_FLAGS)
    builder.uint32("eGL2", OUT_FLAGS_2)
    builder.pstring("eMNA", args.match_name)
    builder.uint32("aeFL", 8)  # AE_RESERVED_INFO
    if args.support_url:
        builder.pstring("eURL", args.support_url)
    return builder.build()


def write_rez_source(path, args, spec_version):
    """The .r file is what macOS builds feed to Rez; kept in sync here."""
    lines = [
        "#include \"AEConfig.h\"",
        "#include \"AE_EffectVers.h\"",
        "",
        "#ifndef AE_OS_WIN",
        "#include \"AE_General.r\"",
        "#endif",
        "",
        "resource 'PiPL' (16000) {",
        "    {",
        "        Kind { AEEffect },",
        "        Name { \"%s\" }," % args.name,
        "        Category { \"%s\" }," % args.category,
        "",
        "#ifdef AE_OS_WIN",
        "    #ifdef AE_PROC_INTELx64",
        "        CodeWin64X86 { \"%s\" }," % ENTRY_POINT,
        "    #endif",
        "#else",
        "    #ifdef AE_OS_MAC",
        "        CodeMacIntel64 { \"%s\" }," % ENTRY_POINT,
        "        CodeMacARM64 { \"%s\" }," % ENTRY_POINT,
        "    #endif",
        "#endif",
        "",
        "        AE_PiPL_Version { 2, 0 },",
        "        AE_Effect_Spec_Version { %d, %d }," % spec_version,
        "        AE_Effect_Version { %d }," % pack_version(args.version_major, args.version_minor,
                                                          args.version_bug, PF_STAGE_RELEASE,
                                                          args.version_build),
        "        AE_Effect_Info_Flags { 0 },",
        "        AE_Effect_Global_OutFlags { %d }," % OUT_FLAGS,
        "        AE_Effect_Global_OutFlags_2 { %d }," % OUT_FLAGS_2,
        "        AE_Effect_Match_Name { \"%s\" }," % args.match_name,
        "        AE_Reserved_Info { 8 }",
        "    }",
        "};",
        "",
    ]
    with open(path, "w", encoding="ascii") as handle:
        handle.write("\n".join(lines))


def write_flags_header(path):
    lines = [
        "#pragma once",
        "",
        "// Generated by tools/generate_pipl.py - do not edit.",
        "// These must match the flags set during PF_Cmd_GLOBAL_SETUP.",
        "#define AB_GLOW_OUT_FLAGS %dL" % OUT_FLAGS,
        "#define AB_GLOW_OUT_FLAGS2 %dL" % OUT_FLAGS_2,
        "",
    ]
    with open(path, "w", encoding="ascii") as handle:
        handle.write("\n".join(lines))


def main(argv):
    parser = argparse.ArgumentParser(description="Generate the AB Glow PiPL resource.")
    parser.add_argument("--sdk-root", required=True, help="After Effects SDK root (contains Headers/)")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--name", default="Profound Glow")
    parser.add_argument("--match-name", default="ABBZ ProfoundGlow")
    parser.add_argument("--category", default="AB Tools")
    parser.add_argument("--support-url", default="")
    parser.add_argument("--basename", default="ProfoundGlow")
    parser.add_argument("--version-major", type=int, default=1)
    parser.add_argument("--version-minor", type=int, default=0)
    parser.add_argument("--version-bug", type=int, default=0)
    parser.add_argument("--version-build", type=int, default=1)
    args = parser.parse_args(argv)

    spec_version = read_spec_version(args.sdk_root)
    os.makedirs(args.output_dir, exist_ok=True)

    pipl_path = os.path.join(args.output_dir, "pipl.bin")
    with open(pipl_path, "wb") as handle:
        handle.write(build_pipl(args, spec_version))

    rc_path = os.path.join(args.output_dir, args.basename + "_PiPL.rc")
    with open(rc_path, "w", encoding="ascii") as handle:
        # A file reference keeps the resource compiler from code-page mangling
        # bytes above 0x7F, which a string literal would corrupt.
        handle.write('16000 PiPL DISCARDABLE "pipl.bin"\n')

    write_flags_header(os.path.join(args.output_dir, "AbGlowPiPLFlags.h"))
    write_rez_source(os.path.join(args.output_dir, args.basename + ".r"), args, spec_version)

    print("PiPL: spec version %d.%d, %d bytes" % (spec_version[0], spec_version[1],
                                                  os.path.getsize(pipl_path)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
