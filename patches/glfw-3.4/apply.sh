#!/bin/sh
# Idempotently apply usdtweak's GLFW 3.4 patches inside a FetchContent
# source directory.  Invoked from CMakeLists.txt (PATCH_COMMAND).
#
# Args:
#   $1 = absolute path to patches/glfw-3.4/
#
# A stamp file in the source root prevents re-application across
# re-configures.

set -e

PATCH_DIR="$1"
STAMP=".usdtweak_patches_applied"

if [ -f "$STAMP" ]; then
    echo "GLFW patches already applied (stamp: $STAMP)."
    exit 0
fi

if [ -z "$PATCH_DIR" ] || [ ! -d "$PATCH_DIR" ]; then
    echo "ERROR: patch dir '$PATCH_DIR' not found." >&2
    exit 1
fi

for p in 0001-cursor-mode-transition-callback.patch ; do
    echo "Applying $p"
    patch -p1 -i "$PATCH_DIR/$p"
done

touch "$STAMP"
echo "GLFW patches applied; stamp written."
