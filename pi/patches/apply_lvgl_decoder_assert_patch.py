#!/usr/bin/env python3
"""Idempotently soft-fail LVGL's Invalid draw buffer assert in decoder open.

LVGL v9.5.0 lv_image_decoder_open() does:

  LV_ASSERT_MSG(decoded->unaligned_data && decoded->handlers, "Invalid draw buffer");

Default LV_ASSERT_HANDLER is `while(1);`. On FlightLevel314 that pinned one
SW draw unit at ~100% CPU with img_decoder_open_lock held, freezing the UI
(main stuck in lv_draw_dispatch_wait_for_request) while fetch kept running.
gdb: PC at `b .` immediately after lv_log_add for line 166.

Replace the assert with unlock + LV_RESULT_INVALID so one bad frame skips
the image instead of wedging the process overnight.
"""
from __future__ import annotations

import pathlib
import sys

MARKER = "Invalid draw buffer — rejecting image open"


def patch(path: pathlib.Path) -> int:
    text = path.read_text()
    if MARKER in text:
        print(f"already patched: {path}")
        return 0

    old = """    if(res == LV_RESULT_OK && dsc->decoded != NULL) {
        LV_ASSERT_MSG(dsc->decoded->unaligned_data && dsc->decoded->handlers, "Invalid draw buffer");

        /* Flush the D-Cache if enabled and the image was successfully opened */
        if(dsc->args.flush_cache) {"""

    new = """    if(res == LV_RESULT_OK && dsc->decoded != NULL) {
        /* FlightLevel314: never LV_ASSERT_HANDLER(while(1)) here — a bad
         * draw buf pinned one SW draw unit at 100% CPU holding the decoder
         * mutex (UI dead, fetch alive, kill -9). Soft-fail the open. */
        if(!(dsc->decoded->unaligned_data && dsc->decoded->handlers)) {
            LV_LOG_ERROR("Invalid draw buffer — rejecting image open");
            lv_mutex_unlock(img_decoder_open_lock_p);
            LV_PROFILER_DECODER_END;
            return LV_RESULT_INVALID;
        }

        /* Flush the D-Cache if enabled and the image was successfully opened */
        if(dsc->args.flush_cache) {"""

    if old not in text:
        print(f"ERROR: Invalid draw buffer assert block not found in {path}", file=sys.stderr)
        return 1

    path.write_text(text.replace(old, new, 1))
    print(f"patched: {path}")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <lvgl-src-root>", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    target = root / "src/draw/lv_image_decoder.c"
    if not target.is_file():
        print(f"ERROR: missing {target}", file=sys.stderr)
        return 1
    return patch(target)


if __name__ == "__main__":
    raise SystemExit(main())
