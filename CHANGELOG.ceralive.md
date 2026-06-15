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
