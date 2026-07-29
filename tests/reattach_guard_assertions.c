/* Assertions for the kernel-driver reattach backstop.
 *
 * The defect under test is defined by what does NOT happen: a process killed
 * with SIGKILL runs no atexit handler, no signal handler, no destructor and no
 * uvc_close(), so every reattach libuvc could perform is skipped and both UVC
 * interfaces are left with no kernel driver for good. A test that merely calls
 * a cleanup function proves nothing about it.
 *
 * So these cases really do SIGKILL a process. The test forks a victim, the
 * victim creates a guard and arms interfaces, then kills ITSELF with an
 * uncatchable signal; the parent asserts the rebind happened anyway. The only
 * thing standing between the victim's death and the assertion is the helper the
 * guard forked, which is the mechanism being claimed.
 *
 * The usbfs operations are swapped for recorders writing into a MAP_SHARED page
 * the whole process tree can see -- the helper is reparented to init and cannot
 * be waited on, so shared memory, not an exit status, is how it reports.
 * `connect_ioctl_is_what_libusb_would_issue` covers the real backend separately
 * by --wrapping ioctl(), so the swap never hides what the kernel is asked for.
 */
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"
#include "libuvc/reattach_guard.h"

#include <errno.h>
#include <linux/usbdevice_fs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
    dump_recorder(); \
    return EXIT_FAILURE; \
  } \
} while (0)

/* Values with no meaning beyond being distinctive in a failure dump. */
#define TEST_BUSNUM 3
#define TEST_DEVNUM 7
#define TEST_ID_VENDOR 0x2ca3
#define TEST_ID_PRODUCT 0x0051
#define TEST_BCD_DEVICE 0x0100

#define MAX_RECORDED_REBINDS 8
#define SETTLE_TIMEOUT_MS 5000
#define SETTLE_POLL_MS 10

struct recorder {
  uint32_t opened;
  uint32_t closed;
  uint32_t rebind_calls;
  /* Number of leading rebind calls that answer EBUSY, so the retry that covers
   * the race with the dying process's descriptor teardown can be observed. */
  uint32_t busy_calls;
  uint32_t rebound_count;
  uint8_t rebound[MAX_RECORDED_REBINDS];
  uint8_t busnum;
  uint8_t devnum;
  uint16_t id_vendor;
  uint16_t id_product;
  uint16_t bcd_device;
};

static struct recorder *recorder;

static void dump_recorder(void) {
  unsigned int i;

  if (recorder == NULL) {
    fprintf(stderr, "  no recorder\n");
    return;
  }

  fprintf(stderr,
          "  recorder: opened=%u closed=%u rebind_calls=%u rebound_count=%u\n",
          recorder->opened, recorder->closed, recorder->rebind_calls,
          recorder->rebound_count);
  fprintf(stderr, "  device seen: bus=%u dev=%u %04x:%04x bcd=%04x\n",
          recorder->busnum, recorder->devnum, recorder->id_vendor,
          recorder->id_product, recorder->bcd_device);
  for (i = 0; i < recorder->rebound_count && i < MAX_RECORDED_REBINDS; i++)
    fprintf(stderr, "    rebound interface %u\n", recorder->rebound[i]);
}

static int recording_open_device(const struct uvc_reattach_record *record) {
  recorder->busnum = record->busnum;
  recorder->devnum = record->devnum;
  recorder->id_vendor = record->id_vendor;
  recorder->id_product = record->id_product;
  recorder->bcd_device = record->bcd_device;
  recorder->opened++;
  return 7;
}

static int recording_rebind(int fd, uint8_t interface_number) {
  (void) fd;

  recorder->rebind_calls++;
  if (recorder->rebind_calls <= recorder->busy_calls)
    return -EBUSY;

  if (recorder->rebound_count < MAX_RECORDED_REBINDS)
    recorder->rebound[recorder->rebound_count] = interface_number;
  recorder->rebound_count++;
  return 0;
}

static void recording_close_device(int fd) {
  (void) fd;
  recorder->closed++;
}

static const uvc_reattach_backend_t recording_backend = {
  recording_open_device,
  recording_rebind,
  recording_close_device
};

static int install_recorder(uint32_t busy_calls) {
  recorder = mmap(NULL, sizeof(*recorder), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (recorder == MAP_FAILED) {
    recorder = NULL;
    return 0;
  }

  memset(recorder, 0, sizeof(*recorder));
  recorder->busy_calls = busy_calls;
  /* Installed before the victim is forked so the helper, two forks away,
   * inherits it. */
  uvc_reattach_guard_set_backend(&recording_backend);
  return 1;
}

static void sleep_ms(int milliseconds) {
  struct timespec request;

  request.tv_sec = milliseconds / 1000;
  request.tv_nsec = (long) (milliseconds % 1000) * 1000000L;
  nanosleep(&request, NULL);
}

/* The helper is reparented to init, so there is nothing to wait on: poll for
 * the rebind it should perform, then give it the same budget again to prove it
 * does not perform any more. */
static void wait_for_rebinds(uint32_t expected) {
  int waited;

  for (waited = 0; waited < SETTLE_TIMEOUT_MS; waited += SETTLE_POLL_MS) {
    if (recorder->rebound_count >= expected && recorder->closed > 0)
      break;
    sleep_ms(SETTLE_POLL_MS);
  }
  sleep_ms(100);
}

/* Runs in a process that is about to be killed without warning. */
typedef void (*victim_body)(uvc_reattach_guard_t *guard);

static int run_victim_then_sigkill(victim_body body) {
  pid_t victim = fork();
  int status = 0;

  if (victim == 0) {
    uvc_reattach_guard_t *guard = uvc_reattach_guard_create(
        TEST_BUSNUM, TEST_DEVNUM, TEST_ID_VENDOR, TEST_ID_PRODUCT,
        TEST_BCD_DEVICE);

    if (guard == NULL)
      _exit(2);

    body(guard);

    /* Uncatchable and unblockable by definition: nothing in this process runs
     * after this line. No uvc_close(), no destroy, no handler -- which is the
     * whole point. */
    raise(SIGKILL);
    _exit(3);
  }

  if (victim < 0)
    return -1;

  while (waitpid(victim, &status, 0) < 0 && errno == EINTR)
    continue;

  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
    return -1;

  return 0;
}

static void arm_two_interfaces(uvc_reattach_guard_t *guard) {
  uvc_reattach_guard_arm(guard, 0);
  uvc_reattach_guard_arm(guard, 1);
}

static void arm_three_and_disarm_two(uvc_reattach_guard_t *guard) {
  uvc_reattach_guard_arm(guard, 0);
  uvc_reattach_guard_arm(guard, 1);
  uvc_reattach_guard_arm(guard, 2);
  uvc_reattach_guard_disarm(guard, 0);
  uvc_reattach_guard_disarm(guard, 1);
}

static void arm_one_interface(uvc_reattach_guard_t *guard) {
  uvc_reattach_guard_arm(guard, 1);
}

/* THE case. A holder killed with SIGKILL mid-stream is the confirmed trigger
 * for the recurring uvcvideo detach, and the reason a close-path fix cannot be
 * the answer: the close path does not run. Both claimed interfaces must get
 * their driver back regardless. */
static int check_rebinds_after_sigkill(void) {
  CHECK(install_recorder(0));
  CHECK(run_victim_then_sigkill(arm_two_interfaces) == 0);

  wait_for_rebinds(2);

  CHECK(recorder->opened == 1);
  CHECK(recorder->rebound_count == 2);
  CHECK(recorder->rebound[0] == 0);
  CHECK(recorder->rebound[1] == 1);
  CHECK(recorder->closed == 1);
  /* The identity the helper carried across the death, so a reused bus address
   * cannot be mistaken for this device. */
  CHECK(recorder->busnum == TEST_BUSNUM);
  CHECK(recorder->devnum == TEST_DEVNUM);
  CHECK(recorder->id_vendor == TEST_ID_VENDOR);
  CHECK(recorder->id_product == TEST_ID_PRODUCT);
  CHECK(recorder->bcd_device == TEST_BCD_DEVICE);
  return EXIT_SUCCESS;
}

/* The backstop must not become a second teardown path. An interface libuvc has
 * already handed back is disarmed, and re-probing it later would fight whatever
 * legitimately holds it by then. Interface 2 stays armed in the same run, so
 * this also witnesses that the helper woke and acted -- the absence of a rebind
 * for 0 and 1 is a decision, not a no-show. */
static int check_disarmed_interfaces_are_not_rebound(void) {
  CHECK(install_recorder(0));
  CHECK(run_victim_then_sigkill(arm_three_and_disarm_two) == 0);

  wait_for_rebinds(1);

  CHECK(recorder->opened == 1);
  CHECK(recorder->rebound_count == 1);
  CHECK(recorder->rebound[0] == 2);
  CHECK(recorder->closed == 1);
  return EXIT_SUCCESS;
}

/* The helper wakes on the pipe's EOF, which the kernel delivers while the dying
 * process's descriptors are still being closed -- the usbfs one included. Until
 * that one goes, the kernel still sees usbfs holding the interface and answers
 * EBUSY, so a single attempt would lose that race silently. */
static int check_busy_interface_is_retried(void) {
  CHECK(install_recorder(3));
  CHECK(run_victim_then_sigkill(arm_one_interface) == 0);

  wait_for_rebinds(1);

  CHECK(recorder->rebind_calls == 4);
  CHECK(recorder->rebound_count == 1);
  CHECK(recorder->rebound[0] == 1);
  return EXIT_SUCCESS;
}

static int wrapped_ioctl_result;
static int wrapped_ioctl_errno;
static int seen_fd;
static int seen_ifno;
static unsigned long seen_request;
static unsigned int seen_ioctl_code;
static void *seen_data;

int __wrap_ioctl(int fd, unsigned long request, void *argument);

int __wrap_ioctl(int fd, unsigned long request, void *argument) {
  const struct usbdevfs_ioctl *command = argument;

  seen_fd = fd;
  seen_request = request;
  seen_ifno = command->ifno;
  /* The request selects usbfs's ioctl-forwarding entry point; this field is the
   * operation the kernel actually performs, and is the one that matters. */
  seen_ioctl_code = (unsigned int) command->ioctl_code;
  seen_data = command->data;

  if (wrapped_ioctl_result < 0)
    errno = wrapped_ioctl_errno;
  return wrapped_ioctl_result;
}

/* The recording backend above proves the lifecycle; this proves the payload.
 * The helper has to issue exactly what libusb_attach_kernel_driver() issues --
 * a USBDEVFS_CONNECT for that one interface -- because the whole claim is that
 * it performs the call the dying process skipped, not something adjacent. */
static int check_connect_ioctl_is_what_libusb_would_issue(void) {
  const uvc_reattach_backend_t *backend = uvc_reattach_default_backend();

  CHECK(backend != NULL);

  wrapped_ioctl_result = 0;
  CHECK(backend->rebind(11, 5) == 0);
  CHECK(seen_fd == 11);
  CHECK(seen_request == (unsigned long) USBDEVFS_IOCTL);
  CHECK(seen_ioctl_code == (unsigned int) USBDEVFS_CONNECT);
  CHECK(seen_ioctl_code != (unsigned int) USBDEVFS_DISCONNECT);
  CHECK(seen_ifno == 5);
  CHECK(seen_data == NULL);

  /* EBUSY has to survive as EBUSY: it is the one answer the retry loop treats
   * as "not finished" rather than "nothing more to do". */
  wrapped_ioctl_result = -1;
  wrapped_ioctl_errno = EBUSY;
  CHECK(backend->rebind(11, 5) == -EBUSY);

  wrapped_ioctl_errno = ENODEV;
  CHECK(backend->rebind(11, 5) == -ENODEV);
  return EXIT_SUCCESS;
}

int __wrap_libusb_detach_kernel_driver(libusb_device_handle *handle, int idx);
int __wrap_libusb_claim_interface(libusb_device_handle *handle, int idx);
int __wrap_libusb_release_interface(libusb_device_handle *handle, int idx);
int __wrap_libusb_set_interface_alt_setting(libusb_device_handle *handle,
                                            int idx, int alternate_setting);
int __wrap_libusb_attach_kernel_driver(libusb_device_handle *handle, int idx);
int __wrap_libusb_get_device_descriptor(libusb_device *device,
                                        struct libusb_device_descriptor *desc);
uint8_t __wrap_libusb_get_bus_number(libusb_device *device);
uint8_t __wrap_libusb_get_device_address(libusb_device *device);

#define MAX_RECORDED_DETACHES 4

/* The claim path's seam. A real device fails a claim only when something else
 * is racing it, which is not something a test can ask for -- so the outcomes of
 * detach, claim and attach are dictated here, and what libuvc did about them is
 * recorded. */
static int forced_detach_result = LIBUSB_SUCCESS;
static int forced_claim_result = LIBUSB_SUCCESS;
static int forced_attach_result = LIBUSB_SUCCESS;
static unsigned int detach_calls;
static unsigned int claim_calls;
static unsigned int attach_calls;
static int last_attached_interface = -1;
/* Sampled at the moment the kernel driver is taken away: the ordering IS the
 * fix, and nothing observable after the call returns can distinguish "armed
 * before the detach" from "armed after the claim". */
static uvc_device_handle_t *observed_devh;
static int guard_existed_at_detach[MAX_RECORDED_DETACHES];
static uint32_t armed_mask_at_detach[MAX_RECORDED_DETACHES];

static void reset_claim_recording(void) {
  forced_detach_result = LIBUSB_SUCCESS;
  forced_claim_result = LIBUSB_SUCCESS;
  forced_attach_result = LIBUSB_SUCCESS;
  detach_calls = 0;
  claim_calls = 0;
  attach_calls = 0;
  last_attached_interface = -1;
  memset(guard_existed_at_detach, 0, sizeof(guard_existed_at_detach));
  memset(armed_mask_at_detach, 0, sizeof(armed_mask_at_detach));
}

int __wrap_libusb_detach_kernel_driver(libusb_device_handle *handle, int idx) {
  (void) handle;
  (void) idx;

  if (observed_devh != NULL && detach_calls < MAX_RECORDED_DETACHES) {
    guard_existed_at_detach[detach_calls] = observed_devh->reattach_guard != NULL;
    armed_mask_at_detach[detach_calls] =
        uvc_reattach_guard_armed_mask(observed_devh->reattach_guard);
  }
  detach_calls++;
  return forced_detach_result;
}

int __wrap_libusb_claim_interface(libusb_device_handle *handle, int idx) {
  (void) handle;
  (void) idx;

  claim_calls++;
  return forced_claim_result;
}

int __wrap_libusb_release_interface(libusb_device_handle *handle, int idx) {
  (void) handle;
  (void) idx;
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_set_interface_alt_setting(libusb_device_handle *handle,
                                            int idx, int alternate_setting) {
  (void) handle;
  (void) idx;
  (void) alternate_setting;
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_attach_kernel_driver(libusb_device_handle *handle, int idx) {
  (void) handle;

  attach_calls++;
  last_attached_interface = idx;
  return forced_attach_result;
}

int __wrap_libusb_get_device_descriptor(libusb_device *device,
                                        struct libusb_device_descriptor *desc) {
  (void) device;
  memset(desc, 0, sizeof(*desc));
  desc->idVendor = TEST_ID_VENDOR;
  desc->idProduct = TEST_ID_PRODUCT;
  desc->bcdDevice = TEST_BCD_DEVICE;
  return LIBUSB_SUCCESS;
}

uint8_t __wrap_libusb_get_bus_number(libusb_device *device) {
  (void) device;
  return TEST_BUSNUM;
}

uint8_t __wrap_libusb_get_device_address(libusb_device *device) {
  (void) device;
  return TEST_DEVNUM;
}

static int inert_open_device(const struct uvc_reattach_record *record) {
  (void) record;
  return -1;
}

static const uvc_reattach_backend_t inert_backend = {
  inert_open_device,
  recording_rebind,
  recording_close_device
};

/* Enough of a handle for uvc_claim_if()/uvc_release_if(): every libusb entry
 * point they reach is --wrapped above, so none of this has to be real. */
static uvc_device_handle_t *make_devh(uvc_context_t *ctx, uvc_device_t *dev) {
  uvc_device_handle_t *devh = calloc(1, sizeof(*devh));

  if (devh == NULL)
    return NULL;

  memset(ctx, 0, sizeof(*ctx));
  memset(dev, 0, sizeof(*dev));
  dev->ctx = ctx;
  dev->usb_dev = (libusb_device *) dev;
  devh->dev = dev;

  reset_claim_recording();
  observed_devh = devh;
  return devh;
}

/* The cases above exercise the guard directly. This one exercises the wiring
 * that puts real interfaces under it: an interface enters the armed set at the
 * moment libuvc takes it from the kernel driver, and leaves only once libuvc
 * has genuinely handed it back. Without this, arming could be deleted from
 * uvc_claim_if() and every other case here would still pass. Arbitrary,
 * non-adjacent interface numbers because nothing may assume a layout. */
static int check_claiming_an_interface_arms_the_guard(void) {
  static uvc_context_t ctx;
  static uvc_device_t dev;
  uvc_device_handle_t *devh;

  CHECK(install_recorder(0));
  uvc_reattach_guard_set_backend(&inert_backend);

  devh = make_devh(&ctx, &dev);
  CHECK(devh != NULL);

  CHECK(uvc_claim_if(devh, 1) == UVC_SUCCESS);
  CHECK(devh->reattach_guard != NULL);
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard) == (1u << 1));

  CHECK(uvc_claim_if(devh, 4) == UVC_SUCCESS);
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard)
        == ((1u << 1) | (1u << 4)));

  /* Armed BEFORE the driver is taken, not after the claim returns. Between
   * those two moments the interface is already this process's responsibility --
   * the detach can land and the claim still fail -- so a backstop that only
   * learns about the interface afterwards cannot cover the interval in which it
   * became driverless. */
  CHECK(guard_existed_at_detach[0] == 1);
  CHECK(armed_mask_at_detach[0] == (1u << 1));
  CHECK(armed_mask_at_detach[1] == ((1u << 1) | (1u << 4)));

  CHECK(uvc_release_if(devh, 1) == UVC_SUCCESS);
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard) == (1u << 4));

  CHECK(uvc_release_if(devh, 4) == UVC_SUCCESS);
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard) == 0);
  return EXIT_SUCCESS;
}

/* THE case for a process that is alive and well. Detaching the kernel driver
 * and claiming the interface are two separate kernel calls, and the first can
 * succeed while the second fails -- a device that is busy, a kernel racing the
 * same interface. The interface is then bound to nothing, and no later libuvc
 * call comes back for it: uvc_release_if() skips anything absent from
 * devh->claimed, so the driver stays off for the life of the process. Measured
 * on hardware as 83 seconds of `1.0=usbfs, 1.1=NONE` with nothing killed.
 * uvc_claim_if() has to undo its own detach. */
static int check_claim_failure_after_detach_reattaches(void) {
  static uvc_context_t ctx;
  static uvc_device_t dev;
  uvc_device_handle_t *devh;

  CHECK(install_recorder(0));
  uvc_reattach_guard_set_backend(&inert_backend);

  devh = make_devh(&ctx, &dev);
  CHECK(devh != NULL);
  forced_claim_result = LIBUSB_ERROR_BUSY;

  CHECK(uvc_claim_if(devh, 3) == UVC_ERROR_BUSY);
  CHECK(detach_calls == 1);
  CHECK(claim_calls == 1);
  CHECK((devh->claimed & (1u << 3)) == 0);

  /* The repair, in this process, now -- not after something dies. */
  CHECK(attach_calls == 1);
  CHECK(last_attached_interface == 3);

  /* The guard is created by the arm itself, so its mere existence proves the
   * interface was armed: before this fix a failed claim left this NULL. Reading
   * it together with a zero mask separates "armed, then correctly disarmed once
   * the driver was back" from "never armed at all", which the mask alone
   * cannot. */
  CHECK(devh->reattach_guard != NULL);
  CHECK(guard_existed_at_detach[0] == 1);
  CHECK(armed_mask_at_detach[0] == (1u << 3));
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard) == 0);
  return EXIT_SUCCESS;
}

/* The backstop half of the same failure. If handing the driver back fails too,
 * the interface really is still driverless and this process cannot fix it --
 * so it must stay armed and let the helper repair it once the usbfs handle is
 * gone. Same rule uvc_release_if() already applies: disarm only on an outcome
 * that means the kernel has the interface back. */
static int check_failed_reattach_after_failed_claim_stays_armed(void) {
  static uvc_context_t ctx;
  static uvc_device_t dev;
  uvc_device_handle_t *devh;

  CHECK(install_recorder(0));
  uvc_reattach_guard_set_backend(&inert_backend);

  devh = make_devh(&ctx, &dev);
  CHECK(devh != NULL);
  forced_claim_result = LIBUSB_ERROR_BUSY;
  forced_attach_result = LIBUSB_ERROR_NO_DEVICE;

  /* The caller learns why the CLAIM failed. The reattach is bookkeeping behind
   * its back and must not overwrite that answer. */
  CHECK(uvc_claim_if(devh, 2) == UVC_ERROR_BUSY);
  CHECK((devh->claimed & (1u << 2)) == 0);
  CHECK(attach_calls == 1);
  CHECK(last_attached_interface == 2);
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard) == (1u << 2));
  return EXIT_SUCCESS;
}

/* Arming before the detach means an interface can be armed that libuvc then
 * never takes. A detach that fails leaves the kernel driver exactly where it
 * was, so there is nothing to hand back and nothing for the helper to repair:
 * the arm has to be rolled back, or every failed claim would leave the helper
 * re-probing an interface someone else legitimately holds. */
static int check_detach_failure_leaves_nothing_armed(void) {
  static uvc_context_t ctx;
  static uvc_device_t dev;
  uvc_device_handle_t *devh;

  CHECK(install_recorder(0));
  uvc_reattach_guard_set_backend(&inert_backend);

  devh = make_devh(&ctx, &dev);
  CHECK(devh != NULL);
  forced_detach_result = LIBUSB_ERROR_NO_DEVICE;

  CHECK(uvc_claim_if(devh, 1) == UVC_ERROR_NO_DEVICE);
  CHECK(detach_calls == 1);
  /* Nothing was taken, so nothing was claimed and nothing was handed back. */
  CHECK(claim_calls == 0);
  CHECK(attach_calls == 0);
  CHECK((devh->claimed & (1u << 1)) == 0);
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard) == 0);
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  CHECK(argc == 3 && strcmp(argv[1], "--case") == 0);
  if (strcmp(argv[2], "rebinds_after_sigkill") == 0)
    return check_rebinds_after_sigkill();
  if (strcmp(argv[2], "disarmed_interfaces_are_not_rebound") == 0)
    return check_disarmed_interfaces_are_not_rebound();
  if (strcmp(argv[2], "busy_interface_is_retried") == 0)
    return check_busy_interface_is_retried();
  if (strcmp(argv[2], "connect_ioctl_is_what_libusb_would_issue") == 0)
    return check_connect_ioctl_is_what_libusb_would_issue();
  if (strcmp(argv[2], "claiming_an_interface_arms_the_guard") == 0)
    return check_claiming_an_interface_arms_the_guard();
  if (strcmp(argv[2], "claim_failure_after_detach_reattaches") == 0)
    return check_claim_failure_after_detach_reattaches();
  if (strcmp(argv[2], "failed_reattach_after_failed_claim_stays_armed") == 0)
    return check_failed_reattach_after_failed_claim_stays_armed();
  if (strcmp(argv[2], "detach_failure_leaves_nothing_armed") == 0)
    return check_detach_failure_leaves_nothing_armed();
  fprintf(stderr, "unknown case: %s\n", argv[2]);
  return EXIT_FAILURE;
}
