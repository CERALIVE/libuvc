# CeraLive/libuvc changelog

This file records changes made by CeraLive on top of the upstream base. For
the upstream history, see `changelog.txt`.

## Fork base

- **Upstream:** `libuvc/libuvc`
- **Base SHA:** `68d07a00e11d1944e27b7295ee69673239c00b4b` (v0.0.7, 2023-03-31)
- **Sync policy:** Hard divergence. CeraLive owns all future fixes. This fork is
  NOT tracked against, pulled from, or rebased onto upstream main. The base SHA
  is recorded for provenance only.
- **License:** BSD-3-Clause, preserved verbatim (`LICENSE.txt`). CeraLive
  additions are also BSD-3-Clause.

## ceralive-v0.0.7.5

Security/correctness hotfix on top of `ceralive-v0.0.7.4`. Extends the round-1
stream-handle quarantine to the **device handle**, off any healthy-device path —
byte-identical streaming behavior for currently-working devices. Fixes a second,
deeper use-after-free that the round-1 fix did not cover.

### Fixed

- **Use-after-free of the device handle on the stop-timeout / late-completion
  path** (fork commit — see tag `ceralive-v0.0.7.5`). The `ceralive-v0.0.7.4`
  fix quarantines (intentionally leaks) the *stream* handle `strmh` when
  `uvc_stream_stop()`'s bounded wait times out with libusb transfers still
  outstanding, so a late `_uvc_stream_callback()` lands on live memory. But
  `strmh->devh` — the `uvc_device_handle_t` — is a **separate** object, and
  `uvc_close()` still freed it via `uvc_free_devh()` after `uvc_stop_streaming()`
  returned, regardless of any quarantined stream. A late completion that arrives
  as `LIBUSB_TRANSFER_COMPLETED` (real data can still land after
  `libusb_cancel_transfer()` — cancellation is asynchronous) re-enters
  `_uvc_process_payload()`, which dereferences `strmh->devh->is_isight`
  (`stream.c:740`/`:754`) **before** it checks `strmh->running`. Freeing `devh`
  in `uvc_close()` therefore left that dereference reading freed memory — a
  use-after-free of the same severity class as the round-1 defect and
  CVE-2026-1991.

  `uvc_stream_close()` now sets a `has_quarantined_stream` flag on `strmh->devh`
  whenever it quarantines a stream. `uvc_close()`, after `uvc_stop_streaming()`
  returns, checks that flag: if set, it **quarantines the device handle too** —
  intentionally leaking `devh` and everything it owns (the USB handle, the libusb
  device reference, the parsed device info) instead of running
  `uvc_release_if()` / `libusb_close()` / `uvc_unref_device()` /
  `uvc_free_devh()`. The handle is still unlinked from `ctx->open_devices` first
  (safe: a late callback reaches `devh` only via `transfer->user_data -> strmh ->
  devh`, never by walking `open_devices`; the unlink keeps `uvc_exit()` / a later
  `uvc_close()` from re-visiting or double-freeing the quarantined handle).
  Skipping `libusb_close()` also avoids closing a USB handle with a transfer
  still outstanding and avoids joining a possibly-wedged event-handler thread —
  preserving the bounded-wait guarantee (never hang forever on a dead device).
  The common fast-cancel path still fully frees **both** `strmh` and `devh` (no
  leak). The quarantine is a small, rare, bounded leak that occurs only on a
  genuinely dead device whose cancelled transfers never completed.

  Verified with a fork-level ASan/LSan harness driving the real `uvc_close()` on
  a stuck-stream device: the round-1 stream quarantine still holds (bounded, late
  `CANCELLED` callback ASan-clean); the normal path fully frees both handles
  (LSan-clean); `uvc_close()` on a stuck-stream device does **not** free `devh`
  and a late `LIBUSB_TRANSFER_COMPLETED` callback dereferencing
  `strmh->devh->is_isight` afterward is ASan-clean; and a revert-check against
  the round-1-only code (`ceralive-v0.0.7.4`) reproduces the use-after-free at
  `_uvc_process_payload()` (`stream.c:740`), freed by `uvc_free_devh()`
  (non-vacuous — round 1 alone is insufficient).

## ceralive-v0.0.7.4

Security/correctness hotfix on top of `ceralive-v0.0.7.3`. One targeted fix in
`src/stream.c`, off any healthy-device path — byte-identical streaming behavior
for currently-working devices. Fixes a use-after-free introduced by the
interaction of the `ceralive-v0.0.7.3` bounded stream-stop (A5, `ab49e21`) with
`uvc_stream_close()`'s unconditional teardown.

### Fixed

- **Use-after-free in `uvc_stream_close()` on the A5 stop-timeout path**
  (fork commit — see tag `ceralive-v0.0.7.4`). A5 made `uvc_stream_stop()` return
  `UVC_ERROR_TIMEOUT` after a bounded wait instead of hanging when a
  dead/unplugged device's cancelled transfers never complete (a wedged libusb
  event thread). On that timeout the `strmh->transfers[i]` slots stay non-NULL
  and libusb still holds the handle via each pending transfer's `user_data`.
  `uvc_stream_close()` discarded the stop result and unconditionally freed
  `strmh`, destroyed its `cb_mutex`/`cb_cond`, and freed its buffers — so a late
  `_uvc_stream_callback()` firing for a still-live transfer dereferenced freed
  memory and locked a destroyed mutex (a use-after-free of the same severity
  class as CVE-2026-1991).

  `uvc_stream_close()` now scans the transfer slots under `cb_mutex` after
  `uvc_stream_stop()` returns. The handle is unconditionally unlinked from
  `devh->streams` (safe: `_uvc_stream_callback()` reaches the handle only via
  `transfer->user_data`, never by walking the list, and unlinking keeps
  `uvc_close()`'s `uvc_stop_streaming()` sweep from re-visiting it). If any
  transfer slot is still non-NULL, the handle is **quarantined** — intentionally
  leaked, with its mutex/cond/buffers left valid — so a late completion lands on
  live memory instead of freed memory; otherwise the original full free/destroy
  sequence runs unchanged. The quarantine is a small, rare, bounded leak that
  occurs only on a genuinely dead device whose cancelled transfers never
  completed; the common fast-cancel path still fully frees the handle (no leak).
  A5's bounded-wait guarantee (never hang forever on a dead device) is preserved.

  Verified with a fork-level ASan/LSan harness driving `uvc_stream_close()` on a
  stuck stream (never-completing cancel): close returns within A5's ~5 s bound,
  a late `_uvc_stream_callback()` fired afterward is ASan-clean, the fast-cancel
  path is LSan-clean, and a revert-check against the pre-fix code reproduces the
  use-after-free inside `_uvc_stream_callback()` (non-vacuous).

## ceralive-v0.0.7.3

Hardening release. Audited, individually-verified backports from upstream
`libuvc` PRs and the `pupil-labs` / `saki4510t` forks, each landed as a single,
fork-style-adapted commit on `hardening/v0.0.7.3` (branched from `eae7f49`, tag
`ceralive-v0.0.7.2`). Every change is either off-by-default or a pure robustness
guard — no negotiated default changes and byte-identical streaming behavior for
currently-working devices. Each entry cites its **fork-commit SHA** and upstream
provenance. Backlog IDs (A1–A14) refer to the equivalence audit in the plugin
repo's `.omo/evidence/task-1-uvc-camera-compat-stability.md`.

### Added

- **Runtime-configurable USB transfer-buffer count** (A2 — fork `001e8d3`;
  upstream PR #291 `7620d2f`, `a100ee7`). New public API
  `uvc_set_transfer_buffers(uvc_device_handle_t *devh, uint8_t count)`
  (`include/libuvc/libuvc.h`, implemented in `src/stream.c`). The count is stored
  on the device handle and latched by the next `uvc_start_streaming()` /
  `uvc_stream_start()`, sizing the transfer allocation/submit loops. `count == 0`
  (the default, unset state) preserves the prior byte-identical behavior of
  allocating `LIBUVC_NUM_TRANSFER_BUFS` (100) buffers; a non-zero value is clamped
  to `[2, 100]`. Setting the count while a stream on the handle is already running
  is rejected with `UVC_ERROR_BUSY`. The API is pinned device-handle-level rather
  than the PR's global `uvc_stream_set_default_number_of_transport_buffers()`
  setter, and the static transfer arrays are retained (only the loop bounds are
  made configurable). No SONAME change; the existing `uvc_start_streaming()`
  signature is unchanged.

### Fixed

- **Retry USB alt-setting once on transient failure** (A1 — fork `3195bbc`;
  upstream PR #293 `212b85d`). `uvc_stream_start()` now retries
  `libusb_set_interface_alt_setting()` (bounded loop, UVC_DEBUG-logged) before
  failing, so a stream start that loses a single alt-setting negotiation to a
  transient USB error recovers instead of aborting. A persistently failing call
  still propagates the original libusb error via the pre-existing `goto fail`.

- **Free frame-metadata buffer in `uvc_stream_close()`** (A3 — fork `3195bbc`;
  upstream PR #295 `ca65b0b`). The per-frame metadata buffer
  (`strmh->frame.metadata` / `meta_outbuf`) was never freed on stream teardown,
  leaking on every open→start→stop→close cycle. It is now freed alongside
  `frame.data` in a guarded-free block. (Upstream PR #275 is the identical
  duplicate; PR #295 was picked per the audit.)

- **Fail loudly on zero submitted transfers** (A2 — fork `001e8d3`; upstream
  PR #291). `uvc_stream_start()` previously treated `transfer_id >= 0` as success
  in its submit-failure path, so a device that accepted *no* transfers reported
  success and then delivered no frames. The test is now `transfer_id > 0`: when at
  least one transfer is submitted the un-submitted remainder is freed and
  streaming continues with fewer buffers, but when zero transfers are submitted
  the allocated transfers are freed and `UVC_ERROR_IO` is returned.

- **Repair degenerate frame descriptors** (A4 — fork `5df5401`; saki4510t
  `328d14d`). During frame-descriptor parsing in `src/device.c`, a
  `dwMaxVideoFrameBufferSize == 0` (seen on some DJI action cameras) is now
  repaired to a sane fallback, and a `dwDefaultFrameInterval` that is zero or
  outside `[dwMinFrameInterval, dwMaxFrameInterval]` is clamped into range;
  UVC_DEBUG-logged. The guard is strict on the degenerate cases only —
  already-sane descriptors are left byte-identical.

- **Bounded wait in `uvc_stream_stop()`; no more indefinite teardown hang**
  (A5 — fork `ab49e21`; pupil-labs `c534e3d`, upstream PR #59 / issue #152). The
  indefinite `pthread_cond_wait` on transfer cancellation is replaced with a
  bounded `pthread_cond_timedwait` (~1 s per iteration, ~5 s cap) that returns
  `UVC_ERROR_TIMEOUT` if cancellations never complete, while still marking the
  stream stopped so `uvc_stream_close()` can proceed. Transfers still in flight
  are not freed (the d3318ae invariant is preserved); the fast-cancel path is
  unchanged.

- **Zero `GET_MAX` payload fallback** (A7 — fork `69c7da8`; upstream PR #277,
  issue #276). When a device's `GET_MAX` returns
  `dwMaxPayloadTransferSize == 0` during stream negotiation, the negotiated
  control now falls back to the `GET_CUR`/descriptor value instead of propagating
  a zero payload size. Composes with the existing `f4af02a` smaller-max-payload
  handling with no double-handling.

- **Corrupt / oversized payload guards** (A9, subsumes A8 — fork `69c7da8`;
  upstream PR #184 + PR #212 + saki4510t `9e95b8a`). One coherent superset patch
  in the payload/frame path: (1) bounds-check PTS/SCR reads in
  `_uvc_process_payload` before dereferencing (`variable_offset + 4 <= header_len`,
  else zero); (2) grow/realloc guard in `_uvc_populate_frame` to
  `max(hold_bytes, height*step)` with an explicit `return` on allocation failure
  (the fork's `UVC_EXIT` is a no-op in non-debug builds), zero-filling any short
  tail and copying only `hold_bytes` — the DJI/compressed `step == 0` zero-size
  tolerance from A4 is preserved; (3) a `frame_had_errors` flag on
  `struct uvc_stream_handle` set on the payload error bit and on non-zero packet
  status, suppressing delivery of the whole corrupt frame to the callback. The
  three already-present guards (overflow clamp, per-payload error-bit skip,
  per-packet status skip) were kept, not duplicated.

- **Preserve VC-header `dwClockFrequency`** (A12 — fork `9874f4c`; pupil-labs
  `92d2f82`, `74e7a96`). `uvc_parse_vc_header()` now reads `dwClockFrequency`
  into the control-interface info for the `0x0110` and `0x0150` bcdUVC cases
  (previously left zeroed), and `uvc_query_stream_ctrl()` stops zeroing it during
  stream-control parsing. Plumbing only — the clock is not surfaced onto frames
  and there is no PTS behavior change (see the plugin's `scr-investigation.md`).

### Not backported (audit-confirmed already-equivalent)

The following backlog items were evaluated and deliberately **not** landed —
each is already covered by the fork's current state or is out of the fork's
scope. Verdicts are from the todo-1 equivalence audit, independently
re-verified against the branch HEAD.

- **A6 — Composite-device control interface routing** (pupil-labs `9004351`):
  the fork already routes unit control requests to the parsed VideoControl
  interface number (`info->ctrl_if.bInterfaceNumber`, set from the interface
  scan, not hard-coded to 0). The fork base is already at pupil-labs's
  post-fix state; re-picking would be a no-op.
- **A10 — `get_device_descriptor` robustness** (upstream `e001f04`): already
  present as commit `eae7f49` (its message records "+ backport e001f04"); the
  `uvc_scan_control` TIS-detection guard is byte-for-byte equivalent.
- **A11 — Detach only an active kernel driver** (upstream PR #224): covered by
  fork commit `2f32812`, which sets `libusb_set_auto_detach_kernel_driver(…, 1)`
  (libusb detaches only when a driver is actually active) and by
  `uvc_claim_if()` already tolerating the no-active-driver error codes.
- **A13 — `libusb_device` refcount leak** (saki4510t `2596242`): the
  libuvc-portion hunk is comment-only for `uvc_ref_device`/`uvc_unref_device`
  (already correct in the fork) and otherwise touches only the Android-JNI
  `uvc_get_device_with_fd`, which does not exist in this fork. The real leak fix
  lives in Android libusb, out of scope. A 1000× enumerate/free loop is
  ASan/LSan-clean, confirming no pre-existing leak.
- **A14 — Double-probe workaround** (upstream issue #242): implemented in the
  plugin (`gstlibuvch264src`) as an opt-in `QUIRK_DOUBLE_PROBE` vid:pid quirk
  seam that ships with an empty table — not a libuvc fork change.

## ceralive-v0.0.7.1

### Added

- **UVC 1.5 header acceptance.** `uvc_parse_vc_header()` (`src/device.c`) now
  accepts `bcdUVC == 0x0150` in addition to `0x0100`/`0x0110`. Newer UVC devices
  (including some DJI action cameras) report UVC 1.5 and were previously rejected
  with `UVC_ERROR_NOT_SUPPORTED`. Pure additive parser fix; applied
  unconditionally.

- **H.265/HEVC format support.** Added `UVC_FRAME_FORMAT_H265` to
  `enum uvc_frame_format` (`include/libuvc/libuvc.h`), included it in the
  `UVC_FRAME_FORMAT_COMPRESSED` group, registered the `H265` fourcc GUID in the
  format table, and handled it in `_uvc_populate_frame()` (`src/stream.c`).
  Required for H.265 UVC streams. Ported from upstream BELABOX; applied
  unconditionally.

### Changed

- **Kernel-driver auto-detach is now configurable.** `uvc_wrap()`
  (`src/device.c`) calls `libusb_set_auto_detach_kernel_driver(usb_devh, 1)` so
  libuvc can claim interfaces already bound to the `uvcvideo` kernel driver. This
  call is now gated by a build option:

  | Option | Default | Effect |
  |--------|---------|--------|
  | `LIBUVC_AUTO_DETACH_KERNEL_DRIVER` | `ON` | Auto-detach the kernel driver when claiming UVC interfaces (current DJI/UVC capture behavior). |
  | `LIBUVC_AUTO_DETACH_KERNEL_DRIVER=OFF` | — | Do not auto-detach; the consumer manages kernel-driver detach itself. |

  The default is `ON`, preserving prior behavior. The option is surfaced to
  `src/device.c` via the generated `libuvc_config.h`
  (`#cmakedefine01 LIBUVC_AUTO_DETACH_KERNEL_DRIVER`), so disabling it compiles
  the `libusb_set_auto_detach_kernel_driver()` call out entirely.

  ```sh
  # Opt out of auto-detach:
  cmake .. -DLIBUVC_AUTO_DETACH_KERNEL_DRIVER=OFF
  ```
