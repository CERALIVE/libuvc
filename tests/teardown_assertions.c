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

static const char *op_name(op_kind kind) {
  switch (kind) {
  case OP_SUBMIT:  return "submit";
  case OP_CANCEL:  return "cancel";
  case OP_SETALT:  return "set_alt";
  case OP_RELEASE: return "release_if";
  case OP_ATTACH:  return "attach_driver";
  case OP_CLOSE:   return "close";
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
 * interfaces libuvc holds, and a non-zero control endpoint means the status
 * interrupt transfer was submitted and is self-re-arming. */
static uvc_device_handle_t *make_handle(uint32_t claimed, uint8_t ctrl_endpoint) {
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

  info->ctrl_if.bInterfaceNumber = 0;
  info->ctrl_if.bEndpointAddress = ctrl_endpoint;

  devh->dev = &dev;
  devh->info = info;
  devh->claimed = claimed;

  op_count = 0;
  urb_in_flight = 0;
  cancel_requested = 0;
  deliver_callbacks = 1;
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

int main(int argc, char **argv) {
  CHECK(argc == 3 && strcmp(argv[1], "--case") == 0);
  if (strcmp(argv[2], "status_xfer_stops_before_control_release") == 0)
    return check_status_xfer_stops_before_control_release();
  if (strcmp(argv[2], "every_claimed_interface_released_control_last") == 0)
    return check_every_claimed_interface_is_released_control_last();
  if (strcmp(argv[2], "no_status_endpoint_unchanged") == 0)
    return check_handle_without_status_endpoint_is_unchanged();
  if (strcmp(argv[2], "undeliverable_status_xfer_quarantines") == 0)
    return check_undeliverable_status_xfer_quarantines_the_handle();
  fprintf(stderr, "unknown case: %s\n", argv[2]);
  return EXIT_FAILURE;
}
