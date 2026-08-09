#!/usr/bin/env python3
"""Idempotently patch LVGL's DRM driver against the page-flip hang.

LVGL v9.5.0 (and current master) can freeze the UI thread forever inside
drm_flush_wait():
  - drmModeAtomicCommit failure frees drm_dev->req but leaves the dangling
    non-NULL pointer; the next wait polls forever.
  - poll(..., -1) never times out if the page-flip event is missed.

Safe to re-run: skips when the timeout / req=NULL markers are already present.
"""
from __future__ import annotations

import pathlib
import sys

MARKER = "DRM page-flip timed out; dropping pending flip"


def patch(path: pathlib.Path) -> int:
    text = path.read_text()
    if MARKER in text:
        print(f"already patched: {path}")
        return 0

    old_commit = """    ret = drmModeAtomicCommit(drm_dev->fd, drm_dev->req, flags, drm_dev);
    if(ret) {
        LV_LOG_ERROR("drmModeAtomicCommit failed: %s (%d)", strerror(errno), errno);
        drmModeAtomicFree(drm_dev->req);
        return ret;
    }"""

    new_commit = """    ret = drmModeAtomicCommit(drm_dev->fd, drm_dev->req, flags, drm_dev);
    /* NONBLOCK commits can fail with EBUSY under load on Pi; retry once
     * without NONBLOCK so the flip still completes instead of leaving a
     * dangling req that hangs drm_flush_wait forever. */
    if(ret && (flags & DRM_MODE_ATOMIC_NONBLOCK)) {
        flags &= ~DRM_MODE_ATOMIC_NONBLOCK;
        ret = drmModeAtomicCommit(drm_dev->fd, drm_dev->req, flags, drm_dev);
    }
    if(ret) {
        LV_LOG_ERROR("drmModeAtomicCommit failed: %s (%d)", strerror(errno), errno);
        drmModeAtomicFree(drm_dev->req);
        /* Upstream frees req but leaves the dangling non-NULL pointer;
         * drm_flush_wait then polls forever on a freed request. */
        drm_dev->req = NULL;
        return ret;
    }"""

    old_wait = """    while(drm_dev->req) {
        int ret;
        do {
            ret = poll(&pfd, 1, -1);
        } while(ret == -1 && errno == EINTR);

        if(ret > 0)
            drmHandleEvent(drm_dev->fd, &drm_dev->drm_event_ctx);
        else {
            LV_LOG_ERROR("poll failed: %s", strerror(errno));
            return;
        }
    }"""

    new_wait = """    while(drm_dev->req) {
        int ret;
        do {
            /* Finite timeout: a missed page-flip event must not freeze the
             * UI thread (touch dead, status-bar timer stuck, only kill -9). */
            ret = poll(&pfd, 1, 500);
        } while(ret == -1 && errno == EINTR);

        if(ret > 0) {
            drmHandleEvent(drm_dev->fd, &drm_dev->drm_event_ctx);
        }
        else if(ret == 0) {
            LV_LOG_ERROR("DRM page-flip timed out; dropping pending flip");
            if(drm_dev->req) {
                drmModeAtomicFree(drm_dev->req);
                drm_dev->req = NULL;
            }
            return;
        }
        else {
            LV_LOG_ERROR("poll failed: %s", strerror(errno));
            if(drm_dev->req) {
                drmModeAtomicFree(drm_dev->req);
                drm_dev->req = NULL;
            }
            return;
        }
    }"""

    if old_commit not in text:
        print(f"ERROR: commit-failure block not found in {path}", file=sys.stderr)
        return 1
    if old_wait not in text:
        print(f"ERROR: drm_flush_wait poll block not found in {path}", file=sys.stderr)
        return 1

    text = text.replace(old_commit, new_commit, 1)
    text = text.replace(old_wait, new_wait, 1)
    path.write_text(text)
    print(f"patched: {path}")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <lvgl-src-root>", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    target = root / "src/drivers/display/drm/lv_linux_drm.c"
    if not target.is_file():
        print(f"ERROR: missing {target}", file=sys.stderr)
        return 1
    return patch(target)


if __name__ == "__main__":
    raise SystemExit(main())
