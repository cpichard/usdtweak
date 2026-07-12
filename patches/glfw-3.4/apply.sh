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

# Order matters: 0005 patches cocoa_window.m on top of 0002.
for p in 0001-cursor-mode-transition-callback.patch \
         0002-cocoa-skip-warp-on-disable.patch \
         0003-win32-warp-event-guard.patch \
         0004-x11-init-warp-guard.patch \
         0005-cocoa-gcmouse-raw-motion.patch ; do
    echo "Applying $p"
    patch -p1 -i "$PATCH_DIR/$p"
done

touch "$STAMP"
echo "GLFW patches applied; stamp written."
