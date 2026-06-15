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

    git clone https://github.com/libuvc/libuvc
    cd libuvc
    mkdir build
    cd build
    cmake ..
    make && sudo make install

and you're set! If you want to change the build configuration, you can edit `CMakeCache.txt`
in the build directory, or use a CMake GUI to make the desired changes.

There is also `BUILD_EXAMPLE` and `BUILD_TEST` options to enable the compilation of `example` and `uvc_test` programs. To use them, replace the `cmake ..` command above with `cmake .. -DBUILD_TEST=ON -DBUILD_EXAMPLE=ON`.
Then you can start them with `./example` and `./uvc_test` respectively. Note that you need OpenCV to build the later (for displaying image).

## Developing with libuvc

The documentation for `libuvc` can currently be found at https://libuvc.github.io/.

Happy hacking!
