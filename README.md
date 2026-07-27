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
      | jq -e '.tests | length == 19'
    ctest --test-dir build/regression --output-on-failure

The 23 cases are grouped as descriptor (11: `h264`, `h265`,
`truncated_format`, `truncated_frame`, `degenerate_h26x`,
`scanner_vc_header_short`, `scanner_vc_oversized`, `scanner_vc_zero`,
`scanner_vs_header_short`, `scanner_vs_oversized`, `scanner_vs_zero`),
negotiation (5: `h264`, `h265`, `near_match`, `probe_set_error`,
`probe_get_error`), transfer (3: `terminal_statuses`, `retry_success`,
`retry_failure`), and teardown (4: `status_xfer_stops_before_control_release`,
`every_claimed_interface_released_control_last`,
`no_status_endpoint_unchanged`, `undeliverable_status_xfer_quarantines`).
CI runs this suite without camera hardware on Ubuntu 22.04
and Ubuntu 24.04. See
`docs/evidence/uvc-camera-compat-stability.md` for its exact scope.

Adjust the `jq` length assertion above to `23` when running it.

### Device teardown contract

`uvc_close()` owns the whole USB teardown and must keep two invariants that are
not visible from the call site — both are regression-locked by the
`libuvc.teardown.*` cases:

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
