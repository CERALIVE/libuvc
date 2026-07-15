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

## ceralive-v0.0.7.8

Security/correctness hotfix on top of `ceralive-v0.0.7.7`. Fixes a
use-after-free in `uvc_exit()`'s iteration over `ctx->open_devices`. Unlike
rounds 1–4 (which are specific to the stop-timeout quarantine feature), this is
a **pre-existing, general fork/upstream defect** — it reproduces on any context
with two or more devices open, with or without a quarantine — that was surfaced
by the round-3/round-4 review of this exact function. Off any single-device or
already-closed path: no behavior change.

### Fixed

- **Use-after-free iterating `ctx->open_devices` in `uvc_exit()`** (fork commit —
  see tag `ceralive-v0.0.7.8`). `uvc_exit()` walked the open-device list with a
  plain `DL_FOREACH`:

      DL_FOREACH(ctx->open_devices, devh) {
        uvc_close(devh);
      }

  `DL_FOREACH(head, el)` expands to `for(el=head; el; el=el->next)` — it reads
  `el->next` in the loop's **increment step, which runs after the body**.
  `uvc_close()`, on its NORMAL (non-quarantine) path, `DL_DELETE`s `devh` from
  `ctx->open_devices` and then `uvc_free_devh(devh)`s it. So once the body has
  freed the current node, the next iteration's `el->next` read dereferences freed
  memory — a genuine use-after-free, reproducible whenever `uvc_exit()` runs with
  two or more devices open on the same context. It also interacts with the
  quarantine feature: a context carrying one quarantined device (already unlinked
  and intentionally leaked) plus one other live, normally-open device hits exactly
  this UAF when the live device's normal close frees it and the loop then reads
  its `next`.

  The fix mirrors `uvc_stop_streaming()`'s existing `DL_FOREACH_SAFE` over
  `devh->streams` (where `uvc_stream_close()` frees `strmh` the same way):

      uvc_device_handle_t *devh, *devh_tmp;

      DL_FOREACH_SAFE(ctx->open_devices, devh, devh_tmp) {
        uvc_close(devh);
      }

  `DL_FOREACH_SAFE(head, el, tmp)` caches `el->next` in `tmp` **before** the body
  runs, so freeing `el` in the body is safe. Only the iteration macro changed; the
  round-3 quarantine guard (`if (ctx->has_quarantined_device) { …; return; }`)
  after the loop is unchanged in position and logic. An exhaustive sweep of every
  remaining `DL_FOREACH(` (non-`_SAFE`) call site in `src/*.c` confirmed this was
  the only one whose loop body frees or deletes the current element; the rest
  (`uvc_already_open`, `uvc_num_devices`, the diagnostic and descriptor-inventory
  walks) are read-only iterations that never mutate the list they traverse.

  Verified with the round-3/round-4 fork-level ASan/LSan harness extended with two
  scenarios: **T8** — quarantine device A (stop-timeout, so A is unlinked and the
  handler thread kept alive), reopen device B normally on the same context, then
  `uvc_exit()`; and **T9** — the fully-normal case, two devices open with no
  quarantine at all, then `uvc_exit()`. Both are clean on the fixed build (T9 is
  LSan-clean: both devices and the context are freed). A revert-check against
  `ceralive-v0.0.7.7` reproduces a `heap-use-after-free` at `init.c` in `uvc_exit`
  (freed by `uvc_free_devh`) in **both** scenarios — proving the tests are
  non-vacuous and that the defect is not quarantine-specific. T4–T7 pass
  identically on both trees, confirming the fix is surgical.

## ceralive-v0.0.7.7

Security/correctness hotfix on top of `ceralive-v0.0.7.6`. Completes the
quarantine philosophy established in rounds 1–3 — *once a context has been
quarantined, never touch its handler thread's lifetime again* — by closing the
one remaining `uvc_close()` path that still could. Off any healthy-device path:
byte-identical behavior for currently-working devices.

### Fixed

- **Surviving event-handler thread wrongly killed by a later normal close on a
  quarantined context** (fork commit — see tag `ceralive-v0.0.7.7`). The
  `ceralive-v0.0.7.6` fix quarantines the **UVC context** (`has_quarantined_device`)
  when `uvc_stream_stop()`'s bounded wait times out with libusb transfers still
  outstanding, deliberately leaving the per-context `_uvc_handle_events()` thread
  alive (joining a wedged dead device would reintroduce the unbounded hang the
  bounded wait removed) and never clearing the flag (a leaked outstanding transfer
  may still reference `ctx->usb_ctx`). But the **normal** (non-quarantine) path of
  `uvc_close()` still had its original last-device branch —

      if (ctx->own_usb_ctx && ctx->open_devices == devh && devh->next == NULL) {
        ctx->kill_handler_thread = 1;
        libusb_close(devh->usb_devh);
        pthread_join(ctx->handler_thread, NULL);
      }

  — **not** gated on `!ctx->has_quarantined_device`. After a quarantine on device
  A, a reconnect can reopen device B on the same context (round 3 correctly
  suppresses a duplicate handler thread, since A's survivor already services the
  whole libusb context). If B is then closed **normally** — e.g. negotiation or
  stream-start failed before any streaming, so `uvc_stream_stop()` never timed out
  for B and B carries no quarantined stream — `uvc_close(B)` sees B as the only /
  last open device (`open_devices == B && B->next == NULL`) and TRUE-branched into
  `kill_handler_thread = 1` + `pthread_join()`, **killing the surviving handler
  thread round 3 deliberately kept alive**. Because `has_quarantined_device` is
  never cleared, `uvc_open_internal()`'s spawn guard (`&& !has_quarantined_device`)
  then never starts a replacement thread on that context — so every subsequent
  transfer submitted on it never completes, silently breaking all future event
  servicing for the remaining lifetime of the context, with no error surfaced.

  This is reachable from the plugin's real reconnect path: `gst_libuvc_h264_src_
  reconnect()` reuses the element's `uvc_context` across retries, and a reopen
  after a quarantine event that fails negotiation or stream-start calls
  `uvc_close()` on exactly the only-open-device it just opened.

  The fix adds `&& !ctx->has_quarantined_device` to that last-device condition, so
  once a context has been quarantined `uvc_close()` never again sets
  `kill_handler_thread` or joins `ctx->handler_thread`; it takes the `else` branch,
  which still `libusb_close()`s the closing device's **own** USB handle (B's
  resources — USB handle, device ref, parsed info — are fully released as before)
  while sparing only the **shared** surviving handler thread. On a context that was
  never quarantined the branch is byte-identical to before: the last normal close
  still kills and joins the thread, and `uvc_exit()` still `libusb_exit()`s and
  `free()`s the context (no leak). This is the last `kill_handler_thread` /
  `pthread_join(ctx->handler_thread)` site in the library; every other reference to
  the handler thread is either the thread body reading the flag (`init.c:89`) or a
  spawn already gated on `!has_quarantined_device` (`device.c` call site) — so no
  ungated path to killing the shared thread remains. The bounded-wait guarantee is
  preserved (no new join of a possibly-wedged thread is introduced).

  Verified with the round-3 fork-level ASan/LSan harness extended with a new
  scenario (T7): quarantine device A on a context; reopen device B on the same
  context (asserting no duplicate handler thread, per round 3); close B **normally**
  (bounded, no timeout); assert the handler thread is still alive and still
  servicing events (`kill_handler_thread == 0`, a live-thread event-loop probe keeps
  advancing, and a third reopened device C still finds exactly one handler thread).
  A revert-check against `ceralive-v0.0.7.6` reproduces the defect in the same
  scenario — `kill_handler_thread == 1` after the normal close and the event-loop
  probe freezes (the surviving thread joined/killed) — proving the test is
  non-vacuous. Both builds are LSan-clean on the unaffected paths (T4/T5/T6).

## ceralive-v0.0.7.6

Security/correctness hotfix on top of `ceralive-v0.0.7.5`. Extends the round-1
stream-handle and round-2 device-handle quarantines up one more level — to the
**UVC context** and its libusb context / event-handler thread — off any
healthy-device path (byte-identical streaming behavior for currently-working
devices). Fixes a context/thread lifetime hazard the round-2 fix did not cover.

### Fixed

- **Use-after-free / race on the libusb context and duplicate event-handler
  thread after a quarantined close** (fork commit — see tag `ceralive-v0.0.7.6`).
  The `ceralive-v0.0.7.5` fix quarantines (intentionally leaks) the *device*
  handle `devh` when `uvc_stream_stop()`'s bounded wait times out with libusb
  transfers still outstanding. But that quarantine branch of `uvc_close()`
  returns **without** the normal "last device" `kill_handler_thread` +
  `pthread_join()` sequence (joining a wedged event thread would reintroduce the
  unbounded hang the bounded wait removed), so the per-context event thread
  `_uvc_handle_events()` is **still looping** on `ctx->usb_ctx` /
  `ctx->kill_handler_thread` afterward. Two hazards followed from that:

  1. **`uvc_exit()` (the plugin `stop()` path).** After the `DL_FOREACH` close
     loop, `uvc_exit()` unconditionally called `libusb_exit(ctx->usb_ctx)` then
     `free(ctx)` — tearing the libusb context and the `uvc_context_t` struct out
     from under the still-running handler thread. Every `_uvc_handle_events()`
     iteration dereferences both, so this is a use-after-free / race on the
     libusb context itself, reachable on every plugin `stop()` after a quarantine.
  2. **`uvc_open_internal()` (the plugin `reconnect()` path).** It spawns a
     handler thread whenever `ctx->own_usb_ctx && ctx->open_devices == NULL`. The
     quarantine unlinks the only device, emptying `open_devices`, so a reconnect
     reopening a device on the **same** context would start a **second**
     `_uvc_handle_events()` thread — two threads calling
     `libusb_handle_events_completed()` on one libusb context.

  A new `has_quarantined_device` flag on `struct uvc_context` closes both.
  `uvc_close()`'s quarantine branch sets it. `uvc_exit()` then checks it and, if
  set, **leaks the context** — skipping `libusb_exit()` and `free(ctx)` —
  mirroring the strmh/devh quarantine leaks so the running handler thread only
  ever reads live, never-freed memory. `uvc_open_internal()`'s handler-thread
  spawn gains `&& !dev->ctx->has_quarantined_device`, so a reconnect never starts
  a duplicate thread: the surviving first thread already services the whole
  libusb context — `_uvc_handle_events()` operates at the libusb-context level,
  not per-device — including any newly-opened device. The flag is never cleared:
  once any device on a context has been quarantined, a leaked outstanding
  transfer may still reference `usb_ctx`, so the context can never be safely torn
  down. The bounded-wait guarantee is preserved (`uvc_exit()` still returns
  immediately, never joining a wedged thread). The normal path is unchanged:
  `uvc_close()` fully frees `devh`, then `uvc_exit()` (`has_quarantined_device`
  clear) still `libusb_exit()`s and `free()`s the context — no leak.

  Full `ctx → dev → devh → strmh` object-graph reachability was re-traced: in the
  quarantine path nothing in that chain (nor `usb_ctx` / `usb_devh`) is freed once
  the context is leaked, so there is no further dangling chain a late callback or
  the lingering handler thread could dereference.

  Verified with a fork-level ASan/LSan harness driving the real `uvc_exit()` and
  the real handler thread (`_uvc_handle_events`) on a quarantined context:
  `uvc_exit()` returns promptly and does not free the context (the handler thread
  and a main-thread probe read live memory, ASan-clean); a reconnect spawns no
  duplicate handler thread (exactly one ever created); the normal path fully frees
  the context (LSan-clean). A revert-check against `ceralive-v0.0.7.5` reproduces
  the exact use-after-free — ASan `heap-use-after-free READ` in
  `_uvc_handle_events` (`init.c:89`), freed by `uvc_exit` (`init.c:148`) — proving
  the test is non-vacuous.

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
provenance. Backlog IDs (A1–A14) and their durable evidence dispositions are
recorded in `docs/evidence/uvc-camera-compat-stability.md`. The focused
hardware-independent assertions live in `tests/` and are enforced by the
blocking regression gate in `.github/workflows/build.yml`.

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
