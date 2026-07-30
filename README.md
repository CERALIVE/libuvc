# CeraLive/libuvc

This is the **CeraLive** fork of `libuvc`, maintained for the
[`gstlibuvch264src`](https://github.com/CeraLive/gstlibuvch264src) GStreamer
capture element (DJI action cameras and UVC H.264/H.265 devices).

**Forked from `libuvc/libuvc` at SHA `68d07a00e11d1944e27b7295ee69673239c00b4b`
(v0.0.7). Hard divergence — not tracking upstream.** This fork is CeraLive-owned
and is NOT tracked against, pulled from, or rebased onto upstream
`libuvc/libuvc` main. The base SHA above is recorded for provenance only.
See `CHANGELOG.ceralive.md` for the list of CeraLive changes.

CeraLive changes on top of the base:

1. **UVC 1.5 header acceptance** — `uvc_parse_vc_header()` accepts
   `bcdUVC == 0x0150` (unconditional, additive).
2. **H.265/HEVC format support** — `UVC_FRAME_FORMAT_H265` enum, GUID
   registration, and frame handling (unconditional, additive).
3. **Configurable kernel-driver auto-detach** — `uvc_wrap()` calls
   `libusb_set_auto_detach_kernel_driver()`, gated by the CMake option
   **`LIBUVC_AUTO_DETACH_KERNEL_DRIVER` (default `ON`)**. The default
   preserves DJI/UVC capture behavior. To opt out (manage detach yourself):

       cmake .. -DLIBUVC_AUTO_DETACH_KERNEL_DRIVER=OFF

4. **Kernel-driver reattach that survives an uncatchable exit** — detaching a
   driver is undone by a userspace call in the release path, so a process killed
   with `SIGKILL`/`SIGSEGV`/`SIGABRT` never undoes it and leaves both UVC
   interfaces with `driver = NONE` for good. libuvc now forks a small helper
   when it first claims an interface; the helper's wakeup is the pipe EOF the
   kernel delivers on the arming process's death, whatever killed it, and it
   re-probes the interfaces that are still armed. That helper is created with a
   raw `clone(2)`, not `fork()`: glibc's `fork()` runs every `pthread_atfork()`
   handler registered anywhere in the process, so in a host that also links
   libsrt the child ran SRT's handler and blocked forever on an SRT mutex a
   now-vanished thread had held — never reaching the helper, and keeping every
   descriptor the fork had copied until something `SIGKILL`ed it. Gated by the
   CMake option
   **`LIBUVC_REATTACH_GUARD` (default `ON`, Linux only)**:

       cmake .. -DLIBUVC_REATTACH_GUARD=OFF

   See `include/libuvc/reattach_guard.h` for the mechanism and its fail-safe
   properties, and "Device teardown contract" below for how it relates to
   `uvc_close()`.

The library remains **BSD-3-Clause**; see `LICENSE.txt`. CeraLive additions are
also BSD-3-Clause. No license change.

---

`libuvc` is a cross-platform library for USB video devices, built atop `libusb`.
It enables fine-grained control over USB video devices exporting the standard USB Video Class
(UVC) interface, enabling developers to write drivers for previously unsupported devices,
or just access UVC devices in a generic fashion.

## Getting and Building libuvc

Prerequisites: You will need `libusb` and [CMake](http://www.cmake.org/) installed.

To build, you can just run these shell commands:

    git clone https://github.com/CERALIVE/libuvc
    cd libuvc
    mkdir build
    cd build
    cmake ..
    make && sudo make install

and you're set! If you want to change the build configuration, you can edit `CMakeCache.txt`
in the build directory, or use a CMake GUI to make the desired changes.

`BUILD_EXAMPLE` enables the example program. `BUILD_TEST` enables `uvc_test`, an
interactive OpenCV demo that needs a real UVC camera and a display; it is not the
automated test suite. `BUILD_TESTING` enables the default-off, hardware-independent
Linux CTest suite. Configure, build, inspect, and run its static build with:

    cmake -S . -B build/regression \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_BUILD_TARGET=Static \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_EXAMPLE=OFF \
      -DBUILD_TEST=OFF \
      -DBUILD_TESTING=ON
    cmake --build build/regression --parallel
    ctest --test-dir build/regression --show-only=json-v1 \
      | jq -e '.tests | length == 37'
    ctest --test-dir build/regression --output-on-failure

The 37 cases are grouped as descriptor (11: `h264`, `h265`,
`truncated_format`, `truncated_frame`, `degenerate_h26x`,
`scanner_vc_header_short`, `scanner_vc_oversized`, `scanner_vc_zero`,
`scanner_vs_header_short`, `scanner_vs_oversized`, `scanner_vs_zero`),
negotiation (5: `h264`, `h265`, `near_match`, `probe_set_error`,
`probe_get_error`), transfer (3: `terminal_statuses`, `retry_success`,
`retry_failure`), teardown (8: `status_xfer_stops_before_control_release`,
`every_claimed_interface_released_control_last`,
`no_status_endpoint_unchanged`, `sparse_interfaces_control_released_last`,
`high_index_interfaces_released`, `cancel_not_found_still_drains`,
`undeliverable_status_xfer_quarantines`, `quarantined_handle_stays_armed`),
reattach (9: `rebinds_after_sigkill`, `disarmed_interfaces_are_not_rebound`,
`busy_interface_is_retried`, `rebinds_despite_a_foreign_atfork_handler`,
`connect_ioctl_is_what_libusb_would_issue`,
`claiming_an_interface_arms_the_guard`, `claim_failure_after_detach_reattaches`,
`failed_reattach_after_failed_claim_stays_armed`,
`detach_failure_leaves_nothing_armed`), and race
(1: `close_races_status_callback`).

The ten guard cases exist only when `LIBUVC_REATTACH_GUARD` is `ON`; with
`-DLIBUVC_REATTACH_GUARD=OFF` the suite is the 27 cases that predate it, and CI
runs that configuration too so a rollback stays a real rollback. Four of the
reattach cases really do `SIGKILL` a forked victim process — that is the point,
since the defect is defined by cleanup code never running. One of those four
also registers a `pthread_atfork()` handler in the victim and holds its mutex
hostage on a second thread, which is what a host linking libsrt does to the
guard's own fork. The three claim-path cases need no kill at all: they cover a
claim that fails in a process that stays alive, which is a different defect with
the same symptom.

CI runs this suite without camera hardware on Ubuntu 22.04
and Ubuntu 24.04. See
`docs/evidence/uvc-camera-compat-stability.md` for its exact scope.

### Sanitized builds

`LIBUVC_SANITIZE` builds the library **and** the tests with a sanitizer —
instrumenting only the test executable would miss the interesting code, since
the teardown races live in `src/device.c` and run on the libusb event thread:

    cmake -S . -B build/tsan \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_BUILD_TARGET=Static \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_EXAMPLE=OFF \
      -DBUILD_TEST=OFF \
      -DBUILD_TESTING=ON \
      -DLIBUVC_SANITIZE=thread
    cmake --build build/tsan --parallel
    ctest --test-dir build/tsan --output-on-failure \
      -R 'libuvc\.(teardown|race|reattach)\.'

`libuvc.race.close_races_status_callback` is the case this exists for: it drives
`uvc_close()` against a real libusb event thread, and only an instrumented run
can see a missing happens-before edge between them. Keep the `-R` filter — the
descriptor, negotiation and transfer suites `--wrap` `free()`, which a sanitized
build replaces, so including them reports a mismatched allocator rather than
anything about this code. A dedicated CI job runs exactly this.

### Device teardown contract

`uvc_close()` owns the whole USB teardown and must keep five invariants that are
not visible from the call site — all regression-locked by the
`libuvc.teardown.*`, `libuvc.reattach.*` and `libuvc.race.*` cases:

1. **The VideoControl status interrupt transfer is stopped before any interface
   is released.** It re-arms itself from `_uvc_status_callback()`, so a
   resubmission that lands after the release makes usbfs re-claim the interface
   from the kernel driver it was just handed back to; the final `libusb_close()`
   then leaves it bound to nothing and the camera's `/dev/videoN` never returns.
   `status_mutex` keeps the callback's stopping-check and its resubmission atomic
   against that stop.
2. **Every interface in `devh->claimed` is released, VideoControl LAST.** A
   failed negotiation can leave the streaming interface claimed, and reattaching
   the driver to VideoControl is what triggers `uvcvideo`'s probe — a probe that
   claims the streaming interfaces itself, so it must run after they are free.
   The order is derived from `devh->claimed` and
   `info->ctrl_if.bInterfaceNumber`, never assumed: a UVC function may sit at any
   interface index and expose several VideoStreaming interfaces.
3. **`status_xfer_submitted` and `status_xfer_stopping` are read and written
   ONLY under `status_mutex`, on both threads.** They are shared between the
   closing thread and the libusb event thread. `volatile` (what they used to be)
   gives neither atomicity nor a happens-before edge, so the close could observe
   the transfer "done" and free it — and the handle — while the callback was
   still inside both. The mutex's unlock/lock pair is what orders the callback's
   last write before the free.
4. **A cancel reporting `LIBUSB_ERROR_NOT_FOUND` is still waited out.** libusb
   documents that code as *"not in progress, already complete, or already
   cancelled"*, and in the last of those the completion callback has not run yet;
   freeing the transfer there is undefined behaviour by libusb's own contract.
   `_uvc_status_callback()` is therefore the only thing that ever clears
   `status_xfer_submitted`, and the bounded drain is the only way out of the stop.
5. **Every interface libuvc claims is handed back, on every exit — including the
   ones where `uvc_close()` never runs.** Invariants 1–4 all assume the process
   lives long enough to execute them; `SIGKILL`, `SIGSEGV` and a watchdog's
   `SIGABRT` execute nothing at all, and the kernel does not re-probe an
   interface when usbfs's claim is dropped at fd-close. `uvc_claim_if()`
   therefore arms the interface with the reattach guard **before it detaches the
   driver**, and it is disarmed only where the driver is genuinely back — by
   `uvc_release_if()` on a normal teardown, or by `uvc_claim_if()` itself on a
   claim that never completed. The guard's forked helper does the rest from
   outside the process. `uvc_reattach_guard_destroy()` runs from
   `uvc_free_devh()` — the one place the handle is truly released, and reached
   from neither quarantine path.

   **A claim that fails after its detach succeeded undoes the detach on the
   spot.** The detach and the claim are two kernel calls and only the first can
   land: a busy device or a kernel racing the same interface fails the second,
   and the interface is then bound to nothing in a process that is alive and
   well. Nothing revisits it — `uvc_release_if()` skips any interface absent
   from `devh->claimed` — so `uvc_claim_if()` issues the
   `libusb_attach_kernel_driver()` itself and disarms only if that call reports
   the kernel has the interface back. This is the one reattach that is not the
   teardown's, and it is why arming happens at the detach rather than at the
   claim: between those two calls there is a real interval in which this process
   is the reason the interface has no driver.

   **The quarantine paths deliberately reattach nothing, and must stay that
   way.** With a quarantined status transfer libusb still owns a URB on the
   VideoControl status endpoint, so releasing that interface re-creates
   invariant 1's defect exactly; with a quarantined stream the in-flight URB
   rides a VideoStreaming interface, so releasing VideoControl alone would make
   `uvcvideo` probe while usbfs still holds the streaming interfaces and
   register no video node at all (invariant 2, in reverse). Both leave the
   handle armed and let the helper repair the binding once this process — and
   the transfer it was racing — is gone. A quarantine is a leak bounded by the
   process lifetime, not a wedged camera.
   `libuvc.teardown.quarantined_handle_stays_armed` locks that down.

## Developing with libuvc

The documentation for `libuvc` can currently be found at https://libuvc.github.io/.

The GitHub Actions build check uses a bounded compiler cache for both CMake build
variants. It caches only the workspace-local `.ccache` directory, keyed by the
runner, compiler identity, and source/build workflow inputs; the `build/` and
`build-off/` directories are never cached. Local builds do not require ccache, but
you can enable it with CMake's compiler launcher options:

    cmake -B build \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

Happy hacking!
