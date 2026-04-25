#!/usr/bin/env bash
# Build crackme01 in two flavors:
#   crackme01.dbg   — with symbols (sanity check, not given to agent)
#   crackme01       — stripped (the actual RE target)
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-gcc}
CFLAGS="-O1 -fno-inline-small-functions -fno-inline-functions-called-once"

"$CC" $CFLAGS -g  -o crackme01.dbg crackme01.c
"$CC" $CFLAGS -s  -o crackme01     crackme01.c

echo "--- built ---"
file crackme01.dbg crackme01
echo "--- symbol counts (dbg vs stripped) ---"
nm crackme01.dbg 2>/dev/null | wc -l | xargs printf "dbg:      %s symbols\n"
nm crackme01     2>&1 | head -1
