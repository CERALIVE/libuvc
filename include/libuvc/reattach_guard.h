/** @file reattach_guard.h
 * @internal
 * @brief Kernel-driver reattach that survives an uncatchable process death.
 *
 * Claiming a UVC interface through libusb means unbinding whatever kernel driver
 * held it (uvcvideo, in practice). Handing it back is a userspace action:
 * libusb_attach_kernel_driver() from libuvc's release path. A process that dies
 * on SIGKILL, SIGSEGV or SIGABRT runs NO user code, so it never performs it --
 * and the kernel does not re-probe an interface on its own when usbfs's claim is
 * dropped at fd-close. Both UVC interfaces are then left with `driver = NONE`
 * permanently: /dev/videoN never comes back and only a manual bind or a USB
 * unbind/bind recovers the camera.
 *
 * No amount of hardening inside the dying process can fix that, because the
 * defining property of the failure is that the dying process executes nothing.
 * The guard therefore moves the reattach OUT of that process:
 *
 *   - At the first successful interface claim, libuvc forks a tiny helper
 *     (double-forked, so it is reparented to init and the host never sees a
 *     SIGCHLD it did not ask for) and keeps the write end of a pipe.
 *   - The helper blocks on the read end. Its wakeup is the pipe's EOF, which the
 *     kernel delivers when the last write end closes -- and the kernel closes
 *     every descriptor of a dying process unconditionally, for _exit(), abort(),
 *     SIGKILL, SIGSEGV, or a segfault inside an unrelated plugin. There is no
 *     signal to catch, no handler to run and nothing to opt out of.
 *   - On wakeup the helper reopens the device by its usbfs path, verifies it is
 *     still the same device, and asks the kernel to re-probe each interface that
 *     is still armed (USBDEVFS_CONNECT -- exactly what
 *     libusb_attach_kernel_driver() issues).
 *
 * The armed set lives in a MAP_SHARED anonymous mapping, so libuvc updates it
 * with a plain store as interfaces are claimed and released; the helper only
 * ever reads it after the arming process is gone, so the two never race.
 *
 * A normal teardown disarms every interface as it reattaches the driver itself,
 * so the helper wakes with nothing to do and exits. The guard is a backstop, not
 * a second teardown path.
 *
 * Fail-safe by construction: the helper holds no reference to the device while
 * it waits. If the helper is itself killed, behaviour is exactly today's --
 * never worse. If it fires spuriously while libuvc still holds the interface,
 * the kernel answers EBUSY and it gives up.
 *
 * Linux-only, and compiled out entirely by -DLIBUVC_REATTACH_GUARD=OFF. Every
 * entry point below is then a no-op that callers need not guard.
 */
#ifndef LIBUVC_REATTACH_GUARD_H
#define LIBUVC_REATTACH_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Highest bInterfaceNumber the armed bitmask can represent. */
#define UVC_REATTACH_MAX_INTERFACES 32

/** @internal
 * @brief The whole state the helper needs, in memory it shares with libuvc.
 *
 * Written only by the arming process, read only by the helper and only after
 * that process has died, so no synchronization is required beyond the ordering
 * the death itself provides.
 */
struct uvc_reattach_record {
  /** Guards against acting on a mapping that was never initialized. */
  uint32_t magic;
  /** Interfaces still detached, indexed by bInterfaceNumber. */
  uint32_t armed;
  /** usbfs coordinates: /dev/bus/usb/<busnum>/<devnum>. */
  uint8_t busnum;
  uint8_t devnum;
  /** Identity of the device that was claimed, so a helper that wakes after the
   * bus address has been reused rebinds nothing. */
  uint16_t id_vendor;
  uint16_t id_product;
  uint16_t bcd_device;
};

/** @internal
 * @brief The two device operations, swappable so the lifecycle can be tested
 * without USB hardware.
 *
 * Everything here runs in the forked helper, i.e. after a fork() in a
 * multi-threaded process: implementations must use only async-signal-safe calls.
 */
typedef struct {
  /** @return an open descriptor for the recorded device, or < 0 to do nothing. */
  int (*open_device)(const struct uvc_reattach_record *record);
  /** @return 0 once the kernel has re-probed the interface, or -errno. -EBUSY
   * means "still claimed", and is the one error worth retrying. */
  int (*rebind)(int fd, uint8_t interface_number);
  void (*close_device)(int fd);
} uvc_reattach_backend_t;

typedef struct uvc_reattach_guard uvc_reattach_guard_t;

/** @internal
 * @brief Fork the helper for one device and return its handle.
 *
 * @return NULL when the guard is compiled out or the helper could not be
 * started; callers treat that as "no backstop" and carry on.
 */
uvc_reattach_guard_t *uvc_reattach_guard_create(uint8_t busnum, uint8_t devnum,
                                                uint16_t id_vendor,
                                                uint16_t id_product,
                                                uint16_t bcd_device);

/** @internal
 * @brief Record that this interface's kernel driver is libuvc's to hand back.
 */
void uvc_reattach_guard_arm(uvc_reattach_guard_t *guard, int interface_number);

/** @internal
 * @brief Record that libuvc has handed the interface back itself.
 */
void uvc_reattach_guard_disarm(uvc_reattach_guard_t *guard, int interface_number);

/** @internal
 * @brief The interfaces the helper would rebind if the process died right now.
 */
uint32_t uvc_reattach_guard_armed_mask(const uvc_reattach_guard_t *guard);

/** @internal
 * @brief Release the guard, waking the helper.
 *
 * Called only where the device handle is genuinely released, so a guard that
 * still has armed interfaces at this point is a teardown that failed to hand an
 * interface back -- precisely the case the helper exists to repair.
 */
void uvc_reattach_guard_destroy(uvc_reattach_guard_t *guard);

/** @internal
 * @brief Replace the device operations. Test seam; NULL restores the default.
 *
 * Process-global and inherited across the fork, so it must be installed before
 * the guard whose helper should use it is created.
 */
void uvc_reattach_guard_set_backend(const uvc_reattach_backend_t *backend);

/** @internal
 * @brief The real usbfs operations, so a test can assert what the kernel is
 * actually asked for. NULL when the guard is compiled out.
 */
const uvc_reattach_backend_t *uvc_reattach_default_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* !def(LIBUVC_REATTACH_GUARD_H) */
