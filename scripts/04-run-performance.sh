#!/bin/sh
# SPDX-License-Identifier: MIT
# Uses the existing runner and owned disc; no save or asset conversion.
set -eu
export PS2X_KFIV_FIXED_FRAME=1
export PS2X_FRAME_INTERPOLATION="${PS2X_FRAME_INTERPOLATION:-1}"
export PS2X_PRESENT_FPS="${PS2X_PRESENT_FPS:-120}"
export PS2X_INTERPOLATION_VERIFY=0
export PS2X_VU1_LIFT=0
export PS2X_GS_BACKEND="${PS2X_GS_BACKEND:-vulkan}"
exec "$(dirname "$0")/04-run.sh" "$@"
