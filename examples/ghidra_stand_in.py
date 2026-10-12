#!/usr/bin/env python3
"""A stand-in for the GhidraMCP bridge, so example 35 runs without Ghidra.

The real bridge is an MCP server (stdio) in front of a Ghidra project. This script speaks the
same tools over the same transport, standard library only, but keeps a tiny stripped "binary"
in memory: four tools that list, decompile, rename and comment its functions, and a few
compiler-scaffolding functions the agent is told to skip.

  python3 examples/ghidra_stand_in.py --transport stdio

It accepts and ignores `--ghidra-server URL`, so the same command line works for the real bridge.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_demo_server import DemoServer, Tool, ToolFailure  # noqa: E402

# name -> (address, pseudo-C). Three user functions that still carry Ghidra's default names,
# and scaffolding that must be left alone.
FUNCTIONS = {
    "_start": ("00101040", "void _start(void) { __libc_start_main(main); }"),
    "frame_dummy": ("00101080", "void frame_dummy(void) { register_tm_clones(); }"),
    "FUN_00101189": ("00101189", "void FUN_00101189(byte *buf, int len) {\n"
                                 "  for (int i = 0; i < len; i++) buf[i] = buf[i] ^ 0x5a;\n}"),
    "FUN_001011d4": ("001011d4", "uint FUN_001011d4(byte *buf, int len) {\n"
                                 "  uint sum = 5381;\n"
                                 "  for (int i = 0; i < len; i++) sum = sum * 33 + buf[i];\n"
                                 "  return sum;\n}"),
    "FUN_00101230": ("00101230", "int FUN_00101230(int argc, char **argv) {\n"
                                 "  if (argc != 2 || strlen(argv[1]) != 16) { puts(\"usage\"); return 1; }\n"
                                 "  return 0;\n}"),
}
COMMENTS = {}


def list_methods(arguments):
    names = list(FUNCTIONS)
    offset, limit = int(arguments.get("offset", 0)), int(arguments.get("limit", 100))
    return "\n".join("%s @ %s" % (name, FUNCTIONS[name][0]) for name in names[offset:offset + limit])


def decompile_function(arguments):
    if arguments["name"] not in FUNCTIONS:
        raise ToolFailure("no function named %s" % arguments["name"])
    return FUNCTIONS[arguments["name"]][1]


def rename_function(arguments):
    old, new = arguments["old_name"], arguments["new_name"]
    if old not in FUNCTIONS:
        raise ToolFailure("no function named %s" % old)
    FUNCTIONS[new] = (FUNCTIONS[old][0], FUNCTIONS.pop(old)[1].replace(old, new))
    return "Renamed successfully"


def set_decompiler_comment(arguments):
    COMMENTS[arguments["address"]] = arguments["comment"]
    return "Comment set"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ghidra-server")
    parser.add_argument("--transport", default="stdio")
    parser.parse_args()
    text = {"type": "string"}
    DemoServer("ghidra-stand-in", [
        Tool("list_methods", "List the functions of the open program, one per line.",
             {"offset": {"type": "integer"}, "limit": {"type": "integer"}}, [], list_methods),
        Tool("decompile_function", "Decompile a function to pseudo-C by name.",
             {"name": text}, ["name"], decompile_function),
        Tool("rename_function", "Rename a function.",
             {"old_name": text, "new_name": text}, ["old_name", "new_name"], rename_function),
        Tool("set_decompiler_comment", "Set a comment at an address.",
             {"address": text, "comment": text}, ["address", "comment"], set_decompiler_comment),
    ]).run_stdio()


if __name__ == "__main__":
    main()
