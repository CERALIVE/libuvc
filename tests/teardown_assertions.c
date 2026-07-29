/* USB teardown ordering assertions for uvc_close().
 *
 * The kernel-visible defects these lock down are invisible to a pure-logic test:
 * both are about the ORDER of libusb calls a close emits, and about which calls
 * are emitted at all. So every relevant libusb entry point is `--wrap`ped into an
 * ordered operation log, the REAL uvc_close() is driven over a synthetic handle,
 * and the log is asserted.
 *
 * The libusb event thread is modelled WITHOUT a thread: the wraps for the USB
 * calls uvc_close() makes -- plus nanosleep(), which is where its bounded drain
 * waits -- deliver one status-transfer callback each. That is exactly the
 * interleaving a real event thread produces at those points, deterministically,
 * and with no window in which a background thread could touch the handle after
 * uvc_close() frees it.
 *
 * libusb_cancel_transfer() deliberately does NOT deliver: production calls it
 * holding devh->status_mutex, and the callback takes the same (non-recursive)
 * mutex.
 */
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void LIBUSB_CALL _uvc_status_callback(struct libusb_transfer *transfer);

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
    dump_ops(); \
    return EXIT_FAILURE; \
  } \
} while (0)

typedef enum {
  OP_SUBMIT,
  OP_CANCEL,
  OP_CALLBACK,
  OP_SETALT,
  OP_RELEASE,
  OP_ATTACH,
  OP_CLOSE
} op_kind;

typedef struct {
  op_kind kind;
  int iface;
} op_t;

#define MAX_OPS 64

static op_t ops[MAX_OPS];
static int op_count;
static int urb_in_flight;
static int cancel_requested;
static int deliver_callbacks;
static int cancel_returns_not_found;

static const char *op_name(op_kind kind) {
  switch (kind) {
  case OP_SUBMIT:   return "submit";
  case OP_CANCEL:   return "cancel";
  case OP_CALLBACK: return "status_callback";
  case OP_SETALT:   return "set_alt";
  case OP_RELEASE:  return "release_if";
  case OP_ATTACH:   return "attach_driver";
  case OP_CLOSE:    return "close";
  }
  return "?";
}

static void dump_ops(void) {
  int i;
  fprintf(stderr, "  observed teardown sequence (%d ops):\n", op_count);
  for (i = 0; i < op_count; i++)
    fprintf(stderr, "    %2d  %-14s iface=%d\n", i, op_name(ops[i].kind), ops[i].iface);
}

static void record(op_kind kind, int iface) {
  if (op_count < MAX_OPS) {
    ops[op_count].kind = kind;
    ops[op_count].iface = iface;
  }
  op_count++;
}

static int index_of_first(op_kind kind, int iface) {
  int i;
  for (i = 0; i < op_count && i < MAX_OPS; i++)
    if (ops[i].kind == kind && ops[i].iface == iface)
      return i;
  return -1;
}

static int index_of_first_any(op_kind kind) {
  int i;
  for (i = 0; i < op_count && i < MAX_OPS; i++)
    if (ops[i].kind == kind)
      return i;
  return -1;
}

static int index_of_last(op_kind kind) {
  int i, found = -1;
  for (i = 0; i < op_count && i < MAX_OPS; i++)
    if (ops[i].kind == kind)
      found = i;
  return found;
}

static int count_of(op_kind kind) {
  int i, n = 0;
  for (i = 0; i < op_count && i < MAX_OPS; i++)
    if (ops[i].kind == kind)
      n++;
  return n;
}

static struct libusb_transfer *status_xfer;

/* Stand in for one libusb event-thread iteration: hand the URB back and run the
 * real callback, which re-arms it unless uvc_close() told it to stop. */
static void pump_event_thread(void) {
  if (!deliver_callbacks || !urb_in_flight || status_xfer == NULL)
    return;
  urb_in_flight = 0;
  status_xfer->status = cancel_requested ? LIBUSB_TRANSFER_CANCELLED
                                         : LIBUSB_TRANSFER_TIMED_OUT;
  record(OP_CALLBACK, -1);
  _uvc_status_callback(status_xfer);
}

int __wrap_libusb_submit_transfer(struct libusb_transfer *transfer) {
  (void) transfer;
  record(OP_SUBMIT, -1);
  urb_in_flight = 1;
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_cancel_transfer(struct libusb_transfer *transfer) {
  (void) transfer;
  record(OP_CANCEL, -1);
  if (!urb_in_flight)
    return LIBUSB_ERROR_NOT_FOUND;
  cancel_requested = 1;
  /* "already cancelled" is one of the three things libusb reports as NOT_FOUND,
   * and it is the one where the completion callback has NOT run yet. The URB
   * therefore stays in flight here: the cancellation is pending, not finished. */
  if (cancel_returns_not_found)
    return LIBUSB_ERROR_NOT_FOUND;
  return LIBUSB_SUCCESS;
}

int __wrap_nanosleep(const struct timespec *req, struct timespec *rem) {
  (void) req;
  (void) rem;
  pump_event_thread();
  return 0;
}

int __wrap_libusb_set_interface_alt_setting(libusb_device_handle *devh,
                                            int interface_number,
                                            int alternate_setting) {
  (void) devh;
  (void) alternate_setting;
  record(OP_SETALT, interface_number);
  pump_event_thread();
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_release_interface(libusb_device_handle *devh,
                                    int interface_number) {
  (void) devh;
  record(OP_RELEASE, interface_number);
  pump_event_thread();
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_attach_kernel_driver(libusb_device_handle *devh,
                                       int interface_number) {
  (void) devh;
  record(OP_ATTACH, interface_number);
  pump_event_thread();
  return LIBUSB_SUCCESS;
}

void __wrap_libusb_close(libusb_device_handle *devh) {
  (void) devh;
  record(OP_CLOSE, -1);
}

void __wrap_libusb_unref_device(libusb_device *dev) {
  (void) dev;
}

static uvc_context_t ctx;
static uvc_device_t dev;

/* Build the handle uvc_open_internal() would have produced: `claimed` carries the
 * interfaces libuvc holds, `ctrl_iface` is the VideoControl interface number, and
 * a non-zero control endpoint means the status interrupt transfer was submitted
 * and is self-re-arming. */
static uvc_device_handle_t *make_handle_on(uint32_t claimed, uint8_t ctrl_endpoint,
                                           uint8_t ctrl_iface) {
  uvc_device_handle_t *devh = calloc(1, sizeof(*devh));
  uvc_device_info_t *info = calloc(1, sizeof(*info));

  if (devh == NULL || info == NULL) {
    free(devh);
    free(info);
    return NULL;
  }

  memset(&ctx, 0, sizeof(ctx));
  memset(&dev, 0, sizeof(dev));
  dev.ctx = &ctx;
  /* Two refs so uvc_unref_device() never free()s this static. */
  dev.ref = 2;
  /* Not the context owner: uvc_close() then takes the branch that closes only
   * this handle instead of killing and joining an event-handler thread. */
  ctx.own_usb_ctx = 0;

  info->ctrl_if.bInterfaceNumber = ctrl_iface;
  info->ctrl_if.bEndpointAddress = ctrl_endpoint;

  devh->dev = &dev;
  devh->info = info;
  devh->claimed = claimed;

  op_count = 0;
  urb_in_flight = 0;
  cancel_requested = 0;
  deliver_callbacks = 1;
  cancel_returns_not_found = 0;
  status_xfer = NULL;

  if (ctrl_endpoint) {
    status_xfer = libusb_alloc_transfer(0);
    if (status_xfer == NULL) {
      free(info);
      free(devh);
      return NULL;
    }
    status_xfer->user_data = devh;
    devh->status_xfer = status_xfer;
    devh->status_xfer_submitted = 1;
    urb_in_flight = 1;
  }

  DL_APPEND(ctx.open_devices, devh);
  return devh;
}

static uvc_device_handle_t *make_handle(uint32_t claimed, uint8_t ctrl_endpoint) {
  return make_handle_on(claimed, ctrl_endpoint, 0);
}

/* A camera whose VideoControl interface carries a status interrupt endpoint. The
 * URB must stop before the interface it rides on is released: a submission that
 * lands after libusb_attach_kernel_driver() makes usbfs re-claim the interface,
 * evicting the driver, and the following libusb_close() then leaves it bound to
 * nothing at all. */
static int check_status_xfer_stops_before_control_release(void) {
  uvc_device_handle_t *devh = make_handle(1u << 0, 0x81);
  int last_submit, release_ctrl;

  CHECK(devh != NULL);
  uvc_close(devh);

  CHECK(count_of(OP_CANCEL) >= 1);
  release_ctrl = index_of_first(OP_RELEASE, 0);
  CHECK(release_ctrl >= 0);
  last_submit = index_of_last(OP_SUBMIT);
  CHECK(last_submit < release_ctrl);
  CHECK(index_of_first(OP_ATTACH, 0) > release_ctrl);
  CHECK(index_of_last(OP_CLOSE) > index_of_first(OP_ATTACH, 0));
  CHECK(index_of_last(OP_SUBMIT) < index_of_last(OP_CLOSE));
  return EXIT_SUCCESS;
}

/* A negotiation that claimed the streaming interface but never reached
 * uvc_stream_close() must still have it released, BEFORE the control interface:
 * reattaching the driver to control is what makes uvcvideo probe the function,
 * and that probe claims the streaming interfaces itself. */
static int check_every_claimed_interface_is_released_control_last(void) {
  uvc_device_handle_t *devh = make_handle((1u << 0) | (1u << 1), 0);
  int release_stream, release_ctrl;

  CHECK(devh != NULL);
  uvc_close(devh);

  release_stream = index_of_first(OP_RELEASE, 1);
  release_ctrl = index_of_first(OP_RELEASE, 0);
  CHECK(release_stream >= 0);
  CHECK(release_ctrl >= 0);
  CHECK(release_stream < release_ctrl);
  CHECK(index_of_first(OP_ATTACH, 1) >= 0);
  CHECK(index_of_first(OP_ATTACH, 0) > release_ctrl);
  CHECK(count_of(OP_RELEASE) == 2);
  return EXIT_SUCCESS;
}

/* Negative control: a VideoControl interface with no status endpoint (the RØDE
 * HDMI-to-USB-C shape) never submitted a URB, so nothing about its teardown may
 * change. Passes before and after the fix. */
static int check_handle_without_status_endpoint_is_unchanged(void) {
  uvc_device_handle_t *devh = make_handle(1u << 0, 0);

  CHECK(devh != NULL);
  uvc_close(devh);

  CHECK(count_of(OP_SUBMIT) == 0);
  CHECK(count_of(OP_CANCEL) == 0);
  CHECK(count_of(OP_RELEASE) == 1);
  CHECK(index_of_first(OP_RELEASE, 0) >= 0);
  CHECK(index_of_first(OP_ATTACH, 0) > index_of_first(OP_RELEASE, 0));
  CHECK(index_of_last(OP_CLOSE) == op_count - 1);
  return EXIT_SUCCESS;
}

/* Both teardown invariants over an ARBITRARY interface layout. The two cases
 * above pin the reproduction device's shape -- VideoControl at interface 0 with
 * at most one VideoStreaming interface next to it -- which a release loop that
 * simply special-cased index 0 would also satisfy. A UVC function may sit
 * anywhere in a configuration's interface space and expose several
 * VideoStreaming interfaces, so the order has to come from devh->claimed and
 * info->ctrl_if.bInterfaceNumber and nothing else. */
static int check_generic_layout(int ctrl_iface, const int *stream_ifaces,
                                int stream_count, uint8_t ctrl_endpoint) {
  uint32_t claimed = 1u << ctrl_iface;
  uvc_device_handle_t *devh;
  int release_ctrl, i;

  for (i = 0; i < stream_count; i++)
    claimed |= 1u << stream_ifaces[i];

  devh = make_handle_on(claimed, ctrl_endpoint, (uint8_t) ctrl_iface);
  CHECK(devh != NULL);
  uvc_close(devh);

  release_ctrl = index_of_first(OP_RELEASE, ctrl_iface);
  CHECK(release_ctrl >= 0);
  CHECK(count_of(OP_RELEASE) == stream_count + 1);

  for (i = 0; i < stream_count; i++) {
    int release_stream = index_of_first(OP_RELEASE, stream_ifaces[i]);
    CHECK(release_stream >= 0);
    CHECK(release_stream < release_ctrl);
    CHECK(index_of_first(OP_ATTACH, stream_ifaces[i]) > release_stream);
  }

  CHECK(index_of_first(OP_ATTACH, ctrl_iface) > release_ctrl);
  CHECK(index_of_last(OP_SUBMIT) < index_of_first_any(OP_RELEASE));
  CHECK(index_of_last(OP_CLOSE) == op_count - 1);
  return EXIT_SUCCESS;
}

/* VideoControl at a nonzero index, three VideoStreaming interfaces around it,
 * and a status interrupt endpoint -- so the status stop and the release order
 * are both exercised on a layout nothing in the fix can have been tuned to. */
static int check_sparse_interfaces_control_released_last(void) {
  static const int stream_ifaces[] = { 1, 5, 7 };
  return check_generic_layout(3, stream_ifaces, 3, 0x83);
}

/* The same contract far away from the low bits the reproduction used, which is
 * where a release scan that stopped early or assumed contiguity would show up.
 * No status endpoint here, so this isolates the interface walk itself. */
static int check_high_index_interfaces_released(void) {
  static const int stream_ifaces[] = { 9, 17, 24 };
  return check_generic_layout(2, stream_ifaces, 3, 0);
}

/* A cancel that reports LIBUSB_ERROR_NOT_FOUND must still be waited out. libusb
 * documents that code as "not in progress, already complete, OR ALREADY
 * CANCELLED", and in the last of those the completion callback has not run yet:
 * the close would go on to release the interface, free the transfer libusb is
 * still about to complete (undefined behaviour by libusb's own contract) and
 * free the devh that the pending callback dereferences. So the callback has to
 * land BEFORE the first USB operation of the teardown, not somewhere in the
 * middle of it. */
static int check_cancel_not_found_still_drains(void) {
  uvc_device_handle_t *devh = make_handle(1u << 0, 0x81);
  int callback;

  CHECK(devh != NULL);
  cancel_returns_not_found = 1;
  uvc_close(devh);

  CHECK(count_of(OP_CANCEL) == 1);
  callback = index_of_first_any(OP_CALLBACK);
  CHECK(callback >= 0);
  CHECK(callback < index_of_first_any(OP_SETALT));
  CHECK(callback < index_of_first(OP_RELEASE, 0));
  CHECK(count_of(OP_SUBMIT) == 0);
  CHECK(count_of(OP_RELEASE) == 1);
  CHECK(index_of_last(OP_CLOSE) == op_count - 1);
  return EXIT_SUCCESS;
}

/* A wedged event thread must not hang the close, and must not let it free a
 * handle libusb still references through the transfer's user_data. */
static int check_undeliverable_status_xfer_quarantines_the_handle(void) {
  uvc_device_handle_t *devh = make_handle(1u << 0, 0x81);

  CHECK(devh != NULL);
  deliver_callbacks = 0;
  uvc_close(devh);

  CHECK(count_of(OP_CANCEL) == 1);
  CHECK(count_of(OP_RELEASE) == 0);
  CHECK(count_of(OP_CLOSE) == 0);
  CHECK(devh->has_quarantined_status_xfer == 1);
  CHECK(ctx.has_quarantined_device == 1);
  CHECK(ctx.open_devices == NULL);
  return EXIT_SUCCESS;
}

/* Nothing in this file may touch /dev: the guard's helper is only here to be
 * inspected, never to act. */
static int inert_open_device(const struct uvc_reattach_record *record) {
  (void) record;
  return -1;
}

static int inert_rebind(int fd, uint8_t interface_number) {
  (void) fd;
  (void) interface_number;
  return 0;
}

static void inert_close_device(int fd) {
  (void) fd;
}

static const uvc_reattach_backend_t inert_backend = {
  inert_open_device,
  inert_rebind,
  inert_close_device
};

/* The quarantine branch releases nothing, on purpose: libusb still owns a URB
 * on the VideoControl status endpoint, so handing the interface back here would
 * let the next resubmission make usbfs evict the driver it was just given --
 * the very defect the status stop exists to prevent. That leaves the interface
 * detached for as long as this process lives, which is exactly the case the
 * out-of-process backstop covers. So a quarantining close must leave the guard
 * ARMED, and a future "tidy-up" that disarms or destroys it here would silently
 * turn a process-lifetime leak back into a permanently wedged camera. */
static int check_quarantined_handle_stays_armed(void) {
  uvc_device_handle_t *devh = make_handle(1u << 0, 0x81);

  CHECK(devh != NULL);

  uvc_reattach_guard_set_backend(&inert_backend);
  devh->reattach_guard = uvc_reattach_guard_create(1, 2, 0x1234, 0x5678, 0x0100);
  CHECK(devh->reattach_guard != NULL);
  uvc_reattach_guard_arm(devh->reattach_guard, 0);

  deliver_callbacks = 0;
  uvc_close(devh);

  CHECK(devh->has_quarantined_status_xfer == 1);
  CHECK(count_of(OP_RELEASE) == 0);
  CHECK(count_of(OP_ATTACH) == 0);
  CHECK(uvc_reattach_guard_armed_mask(devh->reattach_guard) == (1u << 0));
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  CHECK(argc == 3 && strcmp(argv[1], "--case") == 0);
  if (strcmp(argv[2], "status_xfer_stops_before_control_release") == 0)
    return check_status_xfer_stops_before_control_release();
  if (strcmp(argv[2], "every_claimed_interface_released_control_last") == 0)
    return check_every_claimed_interface_is_released_control_last();
  if (strcmp(argv[2], "no_status_endpoint_unchanged") == 0)
    return check_handle_without_status_endpoint_is_unchanged();
  if (strcmp(argv[2], "sparse_interfaces_control_released_last") == 0)
    return check_sparse_interfaces_control_released_last();
  if (strcmp(argv[2], "high_index_interfaces_released") == 0)
    return check_high_index_interfaces_released();
  if (strcmp(argv[2], "cancel_not_found_still_drains") == 0)
    return check_cancel_not_found_still_drains();
  if (strcmp(argv[2], "undeliverable_status_xfer_quarantines") == 0)
    return check_undeliverable_status_xfer_quarantines_the_handle();
  if (strcmp(argv[2], "quarantined_handle_stays_armed") == 0)
    return check_quarantined_handle_stays_armed();
  fprintf(stderr, "unknown case: %s\n", argv[2]);
  return EXIT_FAILURE;
}
