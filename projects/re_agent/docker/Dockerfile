# Ghidra 11.4.3 + GhidraMCP plugin pre-installed.
# Built FROM blacktop/ghidra so we inherit Java 21 + ghidraRun entrypoint.
# Plugin is patched (extension.properties version) for 11.4.3 — see
# ../README.md for why this works (Ghidra's version check is soft).
#
# Build context layout:
#   docker/Dockerfile
#   docker/plugin/GhidraMCP/{extension.properties, Module.manifest, lib/GhidraMCP.jar}

FROM blacktop/ghidra:11.4.3

# System extensions go in /ghidra/Extensions/Ghidra/<name>/ (verified via
# `find / -path "*Extensions*"` against the base image).
COPY plugin/GhidraMCP /ghidra/Extensions/Ghidra/GhidraMCP

# Plugin's embedded HTTP server (default 8080). The user must still
# enable the plugin once via File → Configure → Configure (one-time;
# ~/.config/ghidra survives via the named volume in compose.yaml).
EXPOSE 8080

# Inherit /entrypoint.sh + cmd "client" from the base image (opens GUI).
