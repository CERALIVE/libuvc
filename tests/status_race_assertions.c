/* uvc_close() versus a REAL libusb event thread.
 *
 * teardown_assertions.c models the event thread without a thread: it delivers
 * completions from inside the wraps, which pins the teardown ORDER but can never
 * exhibit a data race, because only one thread ever exists. The bug this file
 * covers is the opposite kind. status_xfer_submitted is written by
 * _uvc_status_callback() on the libusb event thread and read by
 * uvc_stop_status_xfer() on the closing thread, and both used to do it outside
 * devh->status_mutex. `volatile` made that look synchronized while supplying
 * neither atomicity nor any happens-before edge, so uvc_close() could observe the
 * flag clear and go on to release the interface, close the handle and free both
 * the transfer and the devh while the callback was still inside them.
 *
 * So here the event thread is real, and it re-arms the URB as fast as the
 * production callback lets it while uvc_close() runs concurrently. Two things
 * are then asserted per iteration:
 *
 *   - no submission is recorded at or after the first interface release (the
 *     teardown invariant, now under genuine concurrency rather than a scripted
 *     interleaving), and
 *   - the close never had to quarantine, which is what proves the bounded drain
 *     still completes: holding status_mutex across its sleep would block the very
 *     callback it waits for and turn every single close into a timeout.
 *
 * Under a plain build this is a stress test. Under -DLIBUVC_SANITIZE=thread it is
 * a proof: ThreadSanitizer reports the unsynchronized pair from vector clocks, so
 * it fails on the pre-fix code whether or not the two accesses happen to overlap
 * in wall-clock time, and on x86 they usually do not.
 *
 * Lock order matches production exactly -- devh->status_mutex first, then the
 * stand-in for libusb (bus_mutex) -- so the harness cannot invent an inversion
 * that the real code does not have. The event thread therefore claims the URB
 * under bus_mutex alone and DROPS it before entering the callback.
 */
#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void LIBUSB_CALL _uvc_status_callback(struct libusb_transfer *transfer);

#define ITERATIONS 200

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
    return EXIT_FAILURE; \
  } \
} while (0)

static pthread_mutex_t bus_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Everything libusb would own, guarded by bus_mutex. */
static struct {
  struct libusb_transfer *xfer;
  int urb_in_flight;
  int cancel_requested;
  int stop;
  int submits;
  int first_release_at;
  int last_submit_at;
  int op_count;
} bus;

static int next_op(void) {
  return bus.op_count++;
}

int __wrap_libusb_submit_transfer(struct libusb_transfer *transfer) {
  (void) transfer;
  pthread_mutex_lock(&bus_mutex);
  bus.last_submit_at = next_op();
  bus.urb_in_flight = 1;
  bus.submits++;
  pthread_mutex_unlock(&bus_mutex);
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_cancel_transfer(struct libusb_transfer *transfer) {
  int in_flight;

  (void) transfer;
  pthread_mutex_lock(&bus_mutex);
  next_op();
  in_flight = bus.urb_in_flight;
  bus.cancel_requested = 1;
  pthread_mutex_unlock(&bus_mutex);

  /* Losing the race to the event thread is the interesting case, not an error:
   * libusb reports NOT_FOUND once it has reaped the URB, and the callback for it
   * may still be pending. */
  return in_flight ? LIBUSB_SUCCESS : LIBUSB_ERROR_NOT_FOUND;
}

int __wrap_libusb_release_interface(libusb_device_handle *devh,
                                    int interface_number) {
  (void) devh;
  (void) interface_number;
  pthread_mutex_lock(&bus_mutex);
  if (bus.first_release_at < 0)
    bus.first_release_at = next_op();
  else
    next_op();
  pthread_mutex_unlock(&bus_mutex);
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_set_interface_alt_setting(libusb_device_handle *devh,
                                            int interface_number,
                                            int alternate_setting) {
  (void) devh;
  (void) interface_number;
  (void) alternate_setting;
  pthread_mutex_lock(&bus_mutex);
  next_op();
  pthread_mutex_unlock(&bus_mutex);
  return LIBUSB_SUCCESS;
}

int __wrap_libusb_attach_kernel_driver(libusb_device_handle *devh,
                                       int interface_number) {
  (void) devh;
  (void) interface_number;
  pthread_mutex_lock(&bus_mutex);
  next_op();
  pthread_mutex_unlock(&bus_mutex);
  return LIBUSB_SUCCESS;
}

void __wrap_libusb_close(libusb_device_handle *devh) {
  (void) devh;
  pthread_mutex_lock(&bus_mutex);
  next_op();
  pthread_mutex_unlock(&bus_mutex);
}

void __wrap_libusb_unref_device(libusb_device *dev) {
  (void) dev;
}

/* One real libusb_handle_events() thread: reap whatever URB is in flight and run
 * the production callback, which re-arms it until uvc_close() stops it. */
static void *event_thread_main(void *arg) {
  (void) arg;

  for (;;) {
    struct libusb_transfer *xfer = NULL;
    int cancelled = 0;
    int stop;

    pthread_mutex_lock(&bus_mutex);
    stop = bus.stop;
    if (!stop && bus.urb_in_flight) {
      bus.urb_in_flight = 0;
      cancelled = bus.cancel_requested;
      xfer = bus.xfer;
    }
    pthread_mutex_unlock(&bus_mutex);

    if (stop)
      return NULL;
    if (xfer == NULL) {
      sched_yield();
      continue;
    }

    xfer->status = cancelled ? LIBUSB_TRANSFER_CANCELLED
                             : LIBUSB_TRANSFER_TIMED_OUT;
    _uvc_status_callback(xfer);

    /* A terminal completion is libusb's last word on this transfer; touching
     * either the transfer or the handle after it is the use-after-free the close
     * path is required to prevent. */
    if (cancelled)
      return NULL;
  }
}

/* uvc_close() must start while the event thread is ACTIVELY re-arming, otherwise
 * the stop flag is set before the callback ever runs and the two never overlap.
 * Waiting for the first resubmission is what puts the close into the middle of
 * that loop instead of ahead of it. Bounded so a broken callback cannot hang. */
static int wait_for_resubmission(void) {
  long spins;

  for (spins = 0; spins < 100000000L; spins++) {
    int submits;

    pthread_mutex_lock(&bus_mutex);
    submits = bus.submits;
    pthread_mutex_unlock(&bus_mutex);

    if (submits > 0)
      return 1;
    sched_yield();
  }
  return 0;
}

static uvc_context_t ctx;
static uvc_device_t dev;

static int run_one_iteration(int *submits_out) {
  uvc_device_handle_t *devh = calloc(1, sizeof(*devh));
  uvc_device_info_t *info = calloc(1, sizeof(*info));
  struct libusb_transfer *xfer = libusb_alloc_transfer(0);
  pthread_t event_thread;
  int first_release_at, last_submit_at;

  CHECK(devh != NULL && info != NULL && xfer != NULL);

  memset(&ctx, 0, sizeof(ctx));
  memset(&dev, 0, sizeof(dev));
  dev.ctx = &ctx;
  dev.ref = 2;
  ctx.own_usb_ctx = 0;

  info->ctrl_if.bInterfaceNumber = 0;
  info->ctrl_if.bEndpointAddress = 0x81;

  devh->dev = &dev;
  devh->info = info;
  devh->claimed = 1u << 0;
  devh->status_xfer = xfer;
  devh->status_xfer_submitted = 1;
  pthread_mutex_init(&devh->status_mutex, NULL);

  xfer->user_data = devh;

  memset(&bus, 0, sizeof(bus));
  bus.xfer = xfer;
  bus.urb_in_flight = 1;
  bus.first_release_at = -1;
  bus.last_submit_at = -1;

  DL_APPEND(ctx.open_devices, devh);

  CHECK(pthread_create(&event_thread, NULL, event_thread_main, NULL) == 0);
  CHECK(wait_for_resubmission());

  uvc_close(devh);

  pthread_mutex_lock(&bus_mutex);
  bus.stop = 1;
  pthread_mutex_unlock(&bus_mutex);
  CHECK(pthread_join(event_thread, NULL) == 0);

  pthread_mutex_lock(&bus_mutex);
  *submits_out = bus.submits;
  first_release_at = bus.first_release_at;
  last_submit_at = bus.last_submit_at;
  pthread_mutex_unlock(&bus_mutex);

  CHECK(ctx.has_quarantined_device == 0);
  CHECK(ctx.open_devices == NULL);
  CHECK(first_release_at >= 0);
  CHECK(last_submit_at < first_release_at);

  return EXIT_SUCCESS;
}

/* The event thread re-arming while uvc_close() tears the handle down. */
static int check_close_races_status_callback(void) {
  int iteration, total_submits = 0;

  for (iteration = 0; iteration < ITERATIONS; iteration++) {
    int submits = 0;
    if (run_one_iteration(&submits) != EXIT_SUCCESS) {
      fprintf(stderr, "  failed on iteration %d of %d\n", iteration, ITERATIONS);
      return EXIT_FAILURE;
    }
    total_submits += submits;
  }

  /* Without at least one resubmission the run proved nothing: it would mean the
   * event thread never got to race the close at all. */
  CHECK(total_submits > 0);
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  CHECK(argc == 3 && strcmp(argv[1], "--case") == 0);
  if (strcmp(argv[2], "close_races_status_callback") == 0)
    return check_close_races_status_callback();
  fprintf(stderr, "unknown case: %s\n", argv[2]);
  return EXIT_FAILURE;
}
