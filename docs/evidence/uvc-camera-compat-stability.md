# UVC camera compatibility and stability evidence

This document keeps the A1–A14 audit disposition reproducible in a standalone
clone. It distinguishes the focused CTest gate from historical sanitizer work
and manual camera/plugin validation. No camera or other USB hardware was used
to create or run the 19-case suite; it does not reproduce physical-camera
behavior, USB timing, or quarantine/thread races.

## Focused regression gate

The production code is in `src/device.c` and `src/stream.c`. CMake registers
three fixtures from `tests/`, and `.github/workflows/build.yml` blocks Ubuntu
22.04 and Ubuntu 24.04 jobs on the exact 19-test inventory and its result:

- `tests/descriptor_assertions.c`: `libuvc.descriptor.h264`,
  `libuvc.descriptor.h265`, `libuvc.descriptor.truncated_format`,
  `libuvc.descriptor.truncated_frame`, and
  `libuvc.descriptor.degenerate_h26x`, plus the scanner-boundary cases
  `libuvc.descriptor.scanner_vc_header_short`,
  `libuvc.descriptor.scanner_vc_oversized`,
  `libuvc.descriptor.scanner_vc_zero`,
  `libuvc.descriptor.scanner_vs_header_short`,
  `libuvc.descriptor.scanner_vs_oversized`, and
  `libuvc.descriptor.scanner_vs_zero`.
- `tests/negotiation_assertions.c`: `libuvc.negotiation.h264`,
  `libuvc.negotiation.h265`, `libuvc.negotiation.near_match`,
  `libuvc.negotiation.probe_set_error`, and
  `libuvc.negotiation.probe_get_error`.
- `tests/transfer_assertions.c`: `libuvc.transfer.terminal_statuses`,
  `libuvc.transfer.retry_success`, and `libuvc.transfer.retry_failure`.

Only A4 directly overlaps this focused suite: `degenerate_h26x` asserts the
zero buffer-size and invalid default-interval repair introduced by `5df5401`.
The codec, malformed-descriptor, negotiation-error, and transfer-lifecycle
cases protect other production behavior; they must not be read as automated
coverage of the other A-items.

Before the corresponding production guards were applied, retained red runs
showed `truncated_format` and `truncated_frame` failing because malformed
descriptor lengths were accepted, and `probe_set_error` and `probe_get_error`
failing because control-transfer errors were discarded. The fixes are the
length/order checks in `src/device.c` and error propagation in `src/stream.c`.
With those fixes, all 19 cases pass. This records a red/green disposition, not
a claim that those four defects belong to A1–A14.

On Linux with CMake, a GNU-compatible C compiler, libusb development headers,
and pthreads, reproduce the green result exactly with:

```sh
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
```

Expected green outcome: exactly 19 discovered tests and 19 passes. A red run
returns non-zero and names the failed assertion. Capture local tool versions
with `cmake --version; ctest --version; cc --version; pkg-config --modversion
libusb-1.0`.

## A1–A14 dispositions

| Item | Disposition | Durable repository evidence and limits |
|---|---|---|
| A1 | not covered by this focused gate | Alt-setting retry is production code from `3195bbc` in `src/stream.c`; transient real-USB timing remains hardware scope. |
| A2 | not covered by this focused gate | Transfer-count API and zero-submit handling are in `001e8d3`, `include/libuvc/libuvc.h`, and `src/stream.c`; the 19 cases do not exercise stream startup allocation. |
| A3 | historical sanitizer evidence only | Metadata cleanup is in `3195bbc`; teardown leak validation was retained historical sanitizer scope, not this CTest suite. |
| A4 | repository CTest coverage | Descriptor repair is in `5df5401` and `src/device.c`; `libuvc.descriptor.degenerate_h26x` directly asserts both repairs without hardware. Actual DJI descriptors/capture remain hardware scope. |
| A5 | historical sanitizer evidence only | The bounded stop and later quarantine hardening are recorded by `ab49e21`, `dfba86f`, `a1949ae`, `3ede00e`, `2dcca59`, and `71588db`; the thread/timing harness is not part of this focused gate. |
| A6 | not covered by this focused gate | Existing control-interface routing uses parsed interface state in `src/ctrl.c`; no new backport or focused assertion was required. Composite hardware remains manual scope. |
| A7 | not covered by this focused gate | The zero `GET_MAX` payload fallback is in `69c7da8` and `src/stream.c`; the negotiation cases here do not assert it. |
| A8 | not covered by this focused gate | The corrupt-payload superset in A9 subsumed this item in `69c7da8`; packet payload processing is not exercised here. |
| A9 | not covered by this focused gate | Payload bounds/error handling is in `69c7da8` and `src/stream.c`; callback delivery with real packets remains outside this gate. |
| A10 | not covered by this focused gate | Descriptor robustness was already backported in `eae7f49` in `src/device.c`; these descriptor fixtures do not exercise device enumeration. |
| A11 | hardware/plugin validation only | Auto-detach configuration is in `2f32812`, `CMakeLists.txt`, and `src/device.c`; CI builds both option values, but kernel-driver detach behavior requires hardware/system validation. |
| A12 | not covered by this focused gate | Clock preservation is in `9874f4c` in `src/device.c` and `src/stream.c`; the value is not surfaced onto frames and is not asserted here. |
| A13 | historical sanitizer evidence only | Audit disposition in `9874f4c` found the libuvc refcount path already correct and the Android-only path absent; historical enumeration leak checking was not added to this suite. |
| A14 | hardware/plugin validation only | The opt-in double-probe quirk belongs to the consuming plugin, not this repository; its device table and camera validation remain plugin/manual scope. |

CI and local CTest use synthetic descriptors and wrapped libusb calls. Passing
them establishes deterministic parser, negotiation, and transfer-callback
behavior only. It does not establish end-to-end camera compatibility.
