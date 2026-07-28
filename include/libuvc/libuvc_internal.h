/** @file libuvc_internal.h
  * @brief Implementation-specific UVC constants and structures.
  * @cond include_hidden
  */
#ifndef LIBUVC_INTERNAL_H
#define LIBUVC_INTERNAL_H

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <libusb.h>
#include "utlist.h"
#include "libuvc/reattach_guard.h"

/** Converts an unaligned four-byte little-endian integer into an int32 */
#define DW_TO_INT(p) ((p)[0] | ((p)[1] << 8) | ((p)[2] << 16) | ((p)[3] << 24))
/** Converts an unaligned two-byte little-endian integer into an int16 */
#define SW_TO_SHORT(p) ((p)[0] | ((p)[1] << 8))
/** Converts an int16 into an unaligned two-byte little-endian integer */
#define SHORT_TO_SW(s, p) \
  (p)[0] = (s); \
  (p)[1] = (s) >> 8;
/** Converts an int32 into an unaligned four-byte little-endian integer */
#define INT_TO_DW(i, p) \
  (p)[0] = (i); \
  (p)[1] = (i) >> 8; \
  (p)[2] = (i) >> 16; \
  (p)[3] = (i) >> 24;

/* Used by the degenerate frame-descriptor repair in device.c (saki4510t 328d14d). */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/** Selects the nth item in a doubly linked list. n=-1 selects the last item. */
#define DL_NTH(head, out, n) \
  do { \
    int dl_nth_i = 0; \
    LDECLTYPE(head) dl_nth_p = (head); \
    if ((n) < 0) { \
      while (dl_nth_p && dl_nth_i > (n)) { \
        dl_nth_p = dl_nth_p->prev; \
        dl_nth_i--; \
      } \
    } else { \
      while (dl_nth_p && dl_nth_i < (n)) { \
        dl_nth_p = dl_nth_p->next; \
        dl_nth_i++; \
      } \
    } \
    (out) = dl_nth_p; \
  } while (0);

#ifdef UVC_DEBUGGING
#include <libgen.h>
#ifdef __ANDROID__
#include <android/log.h>
#define UVC_DEBUG(format, ...) __android_log_print(ANDROID_LOG_DEBUG, "libuvc", "[%s:%d/%s] " format "\n", basename(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define UVC_ENTER() __android_log_print(ANDROID_LOG_DEBUG, "libuvc", "[%s:%d] begin %s\n", basename(__FILE__), __LINE__, __FUNCTION__)
#define UVC_EXIT(code) __android_log_print(ANDROID_LOG_DEBUG, "libuvc", "[%s:%d] end %s (%d)\n", basename(__FILE__), __LINE__, __FUNCTION__, code)
#define UVC_EXIT_VOID() __android_log_print(ANDROID_LOG_DEBUG, "libuvc", "[%s:%d] end %s\n", basename(__FILE__), __LINE__, __FUNCTION__)
#else
#define UVC_DEBUG(format, ...) fprintf(stderr, "[%s:%d/%s] " format "\n", basename(__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define UVC_ENTER() fprintf(stderr, "[%s:%d] begin %s\n", basename(__FILE__), __LINE__, __FUNCTION__)
#define UVC_EXIT(code) fprintf(stderr, "[%s:%d] end %s (%d)\n", basename(__FILE__), __LINE__, __FUNCTION__, code)
#define UVC_EXIT_VOID() fprintf(stderr, "[%s:%d] end %s\n", basename(__FILE__), __LINE__, __FUNCTION__)
#endif
#else
#define UVC_DEBUG(format, ...)
#define UVC_ENTER()
#define UVC_EXIT_VOID()
#define UVC_EXIT(code)
#endif

/* http://stackoverflow.com/questions/19452971/array-size-macro-that-rejects-pointers */
#define IS_INDEXABLE(arg) (sizeof(arg[0]))
#define IS_ARRAY(arg) (IS_INDEXABLE(arg) && (((void *) &arg) == ((void *) arg)))
#define ARRAYSIZE(arr) (sizeof(arr) / (IS_ARRAY(arr) ? sizeof(arr[0]) : 0))

/** Video interface subclass code (A.2) */
enum uvc_int_subclass_code {
  UVC_SC_UNDEFINED = 0x00,
  UVC_SC_VIDEOCONTROL = 0x01,
  UVC_SC_VIDEOSTREAMING = 0x02,
  UVC_SC_VIDEO_INTERFACE_COLLECTION = 0x03
};

/** Video interface protocol code (A.3) */
enum uvc_int_proto_code {
  UVC_PC_PROTOCOL_UNDEFINED = 0x00
};

/** VideoControl interface descriptor subtype (A.5) */
enum uvc_vc_desc_subtype {
  UVC_VC_DESCRIPTOR_UNDEFINED = 0x00,
  UVC_VC_HEADER = 0x01,
  UVC_VC_INPUT_TERMINAL = 0x02,
  UVC_VC_OUTPUT_TERMINAL = 0x03,
  UVC_VC_SELECTOR_UNIT = 0x04,
  UVC_VC_PROCESSING_UNIT = 0x05,
  UVC_VC_EXTENSION_UNIT = 0x06
};

/** UVC endpoint descriptor subtype (A.7) */
enum uvc_ep_desc_subtype {
  UVC_EP_UNDEFINED = 0x00,
  UVC_EP_GENERAL = 0x01,
  UVC_EP_ENDPOINT = 0x02,
  UVC_EP_INTERRUPT = 0x03
};

/** VideoControl interface control selector (A.9.1) */
enum uvc_vc_ctrl_selector {
  UVC_VC_CONTROL_UNDEFINED = 0x00,
  UVC_VC_VIDEO_POWER_MODE_CONTROL = 0x01,
  UVC_VC_REQUEST_ERROR_CODE_CONTROL = 0x02
};

/** Terminal control selector (A.9.2) */
enum uvc_term_ctrl_selector {
  UVC_TE_CONTROL_UNDEFINED = 0x00
};

/** Selector unit control selector (A.9.3) */
enum uvc_su_ctrl_selector {
  UVC_SU_CONTROL_UNDEFINED = 0x00,
  UVC_SU_INPUT_SELECT_CONTROL = 0x01
};

/** Extension unit control selector (A.9.6) */
enum uvc_xu_ctrl_selector {
  UVC_XU_CONTROL_UNDEFINED = 0x00
};

/** VideoStreaming interface control selector (A.9.7) */
enum uvc_vs_ctrl_selector {
  UVC_VS_CONTROL_UNDEFINED = 0x00,
  UVC_VS_PROBE_CONTROL = 0x01,
  UVC_VS_COMMIT_CONTROL = 0x02,
  UVC_VS_STILL_PROBE_CONTROL = 0x03,
  UVC_VS_STILL_COMMIT_CONTROL = 0x04,
  UVC_VS_STILL_IMAGE_TRIGGER_CONTROL = 0x05,
  UVC_VS_STREAM_ERROR_CODE_CONTROL = 0x06,
  UVC_VS_GENERATE_KEY_FRAME_CONTROL = 0x07,
  UVC_VS_UPDATE_FRAME_SEGMENT_CONTROL = 0x08,
  UVC_VS_SYNC_DELAY_CONTROL = 0x09
};

/** Status packet type (2.4.2.2) */
enum uvc_status_type {
  UVC_STATUS_TYPE_CONTROL = 1,
  UVC_STATUS_TYPE_STREAMING = 2
};

/** Payload header flags (2.4.3.3) */
#define UVC_STREAM_EOH (1 << 7)
#define UVC_STREAM_ERR (1 << 6)
#define UVC_STREAM_STI (1 << 5)
#define UVC_STREAM_RES (1 << 4)
#define UVC_STREAM_SCR (1 << 3)
#define UVC_STREAM_PTS (1 << 2)
#define UVC_STREAM_EOF (1 << 1)
#define UVC_STREAM_FID (1 << 0)

/** Control capabilities (4.1.2) */
#define UVC_CONTROL_CAP_GET (1 << 0)
#define UVC_CONTROL_CAP_SET (1 << 1)
#define UVC_CONTROL_CAP_DISABLED (1 << 2)
#define UVC_CONTROL_CAP_AUTOUPDATE (1 << 3)
#define UVC_CONTROL_CAP_ASYNCHRONOUS (1 << 4)

struct uvc_streaming_interface;
struct uvc_device_info;

/** VideoStream interface */
typedef struct uvc_streaming_interface {
  struct uvc_device_info *parent;
  struct uvc_streaming_interface *prev, *next;
  /** Interface number */
  uint8_t bInterfaceNumber;
  /** Video formats that this interface provides */
  struct uvc_format_desc *format_descs;
  /** USB endpoint to use when communicating with this interface */
  uint8_t bEndpointAddress;
  uint8_t bTerminalLink;
  uint8_t bStillCaptureMethod;
} uvc_streaming_interface_t;

/** VideoControl interface */
typedef struct uvc_control_interface {
  struct uvc_device_info *parent;
  struct uvc_input_terminal *input_term_descs;
  // struct uvc_output_terminal *output_term_descs;
  struct uvc_selector_unit *selector_unit_descs;
  struct uvc_processing_unit *processing_unit_descs;
  struct uvc_extension_unit *extension_unit_descs;
  uint16_t bcdUVC;
  uint32_t dwClockFrequency;
  uint8_t bEndpointAddress;
  /** Interface number */
  uint8_t bInterfaceNumber;
} uvc_control_interface_t;

struct uvc_stream_ctrl;

struct uvc_device {
  struct uvc_context *ctx;
  int ref;
  libusb_device *usb_dev;
};

typedef struct uvc_device_info {
  /** Configuration descriptor for USB device */
  struct libusb_config_descriptor *config;
  /** VideoControl interface provided by device */
  uvc_control_interface_t ctrl_if;
  /** VideoStreaming interfaces on the device */
  uvc_streaming_interface_t *stream_ifs;
} uvc_device_info_t;

/*
  set a high number of transfer buffers. This uses a lot of ram, but
  avoids problems with scheduling delays on slow boards causing missed
  transfers. A better approach may be to make the transfer thread FIFO
  scheduled (if we have root).
  Default number of transfer buffers can be overwritten by defining
  this macro.
 */
#ifndef LIBUVC_NUM_TRANSFER_BUFS
#if defined(__APPLE__) && defined(__MACH__)
#define LIBUVC_NUM_TRANSFER_BUFS 20
#else
#define LIBUVC_NUM_TRANSFER_BUFS 100
#endif
#endif

/* Bound uvc_stream_stop()'s wait for transfer cancellation so a wedged event
 * thread (e.g. a dead/unplugged device that never completes the cancelled
 * transfers) cannot hang the caller forever. Each iteration waits at most
 * LIBUVC_STREAM_STOP_TIMEOUT_SECS; after LIBUVC_STREAM_STOP_TIMEOUT_ATTEMPTS
 * consecutive timeouts (~5s total) uvc_stream_stop returns UVC_ERROR_TIMEOUT.
 */
#ifndef LIBUVC_STREAM_STOP_TIMEOUT_SECS
#define LIBUVC_STREAM_STOP_TIMEOUT_SECS 1
#endif
#ifndef LIBUVC_STREAM_STOP_TIMEOUT_ATTEMPTS
#define LIBUVC_STREAM_STOP_TIMEOUT_ATTEMPTS 5
#endif

/* Bound uvc_close()'s wait for the VideoControl status interrupt transfer to come
 * back after it is cancelled, mirroring the stream-stop bound above: a wedged
 * event thread must not hang the caller. The endpoint's bInterval is typically
 * 8-32 ms, so a cancellation normally lands within one interval; the default is
 * an order of magnitude above that.
 */
#ifndef LIBUVC_STATUS_STOP_TIMEOUT_MS
#define LIBUVC_STATUS_STOP_TIMEOUT_MS 500
#endif
#ifndef LIBUVC_STATUS_STOP_POLL_MS
#define LIBUVC_STATUS_STOP_POLL_MS 2
#endif

#define LIBUVC_XFER_META_BUF_SIZE ( 4 * 1024 )

struct uvc_stream_handle {
  struct uvc_device_handle *devh;
  struct uvc_stream_handle *prev, *next;
  struct uvc_streaming_interface *stream_if;

  /** if true, stream is running (streaming video to host) */
  uint8_t running;
  /** Current control block */
  struct uvc_stream_ctrl cur_ctrl;

  /* listeners may only access hold*, and only when holding a
   * lock on cb_mutex (probably signaled with cb_cond) */
  uint8_t fid;
  uint32_t seq, hold_seq;
  uint32_t pts, hold_pts;
  uint32_t last_scr, hold_last_scr;
  size_t got_bytes, hold_bytes;
  uint8_t *outbuf, *holdbuf;
  pthread_mutex_t cb_mutex;
  pthread_cond_t cb_cond;
  pthread_t cb_thread;
  uint32_t last_polled_seq;
  uvc_frame_callback_t *user_cb;
  void *user_ptr;
  struct libusb_transfer *transfers[LIBUVC_NUM_TRANSFER_BUFS];
  uint8_t *transfer_bufs[LIBUVC_NUM_TRANSFER_BUFS];
  struct uvc_frame frame;
  enum uvc_frame_format frame_format;
  struct timespec capture_time_finished;

  /* raw metadata buffer if available */
  uint8_t *meta_outbuf, *meta_holdbuf;
  size_t meta_got_bytes, meta_hold_bytes;

  /* set when a payload/packet of the in-progress frame reported an error;
   * a flagged frame is suppressed in _uvc_swap_buffers and never delivered */
  uint8_t frame_had_errors;
};

/** Handle on an open UVC device
 *
 * @todo move most of this into a uvc_device struct?
 */
struct uvc_device_handle {
  struct uvc_device *dev;
  struct uvc_device_handle *prev, *next;
  /** Underlying USB device handle */
  libusb_device_handle *usb_devh;
  struct uvc_device_info *info;
  struct libusb_transfer *status_xfer;
  uint8_t status_buf[32];
  /** Function to call when we receive status updates from the camera */
  uvc_status_callback_t *status_cb;
  void *status_user_ptr;
  /** Function to call when we receive button events from the camera */
  uvc_button_callback_t *button_cb;
  void *button_user_ptr;

  uvc_stream_handle_t *streams;
  /** Whether the camera is an iSight that sends one header per frame */
  uint8_t is_isight;
  uint32_t claimed;
  /** Requested number of USB transfer buffers for the NEXT stream start.
   * 0 = use the library default (LIBUVC_NUM_TRANSFER_BUFS). A non-zero value is
   * clamped to [2, 100] by uvc_set_transfer_buffers() and latched at
   * uvc_stream_start() time. */
  uint8_t transfer_buffer_count;
  /** Set once uvc_stream_close() had to QUARANTINE (intentionally leak) one of
   * this device's streams because uvc_stream_stop()'s bounded wait timed out
   * with libusb transfers still outstanding (a dead/unplugged device whose
   * cancelled transfers never completed). A late _uvc_stream_callback() for such
   * a transfer re-enters _uvc_process_payload(), which dereferences
   * strmh->devh->is_isight -- i.e. THIS handle -- before it checks
   * strmh->running. So while this flag is set uvc_close() must NOT free the
   * handle (uvc_free_devh) or tear down anything it owns; it quarantines the
   * device handle too, mirroring the stream-handle leak. Never cleared. */
  uint8_t has_quarantined_stream;
  /** Serializes _uvc_status_callback()'s decide-and-resubmit against
   * uvc_stop_status_xfer()'s stop-and-cancel. Without the mutex the two
   * interleave: the callback can read status_xfer_stopping as 0, lose the CPU,
   * and issue its resubmission after uvc_stop_status_xfer() has already returned
   * and uvc_close() has released the VideoControl interface -- the exact
   * URB-after-release the stop exists to prevent.
   *
   * It is also the ONLY synchronization for the two flags below: EVERY read and
   * EVERY write of either one, on either thread, is made holding this mutex.
   * That is not bookkeeping tidiness -- it is what gives uvc_close() the
   * happens-before edge it needs. Observing status_xfer_submitted == 0 through
   * this mutex means the event thread's last touch of `status_xfer` and of this
   * handle is ordered BEFORE the release/close/free that follows, because an
   * unlock/lock pair is a release/acquire pair. A bare `volatile` flag (what
   * this used to be) supplies neither atomicity nor that edge: C makes the
   * concurrent accesses a data race outright, and on the weakly-ordered aarch64
   * the fork actually ships on, the closing thread can see the flag clear while
   * the callback's earlier stores are still invisible -- then free the transfer
   * and the handle out from under it.
   *
   * The lock order is uniform and one-way: status_mutex is taken FIRST and libusb
   * entry points are called under it, never the reverse. libusb invokes transfer
   * callbacks with no internal transfer lock held and cancellation is
   * asynchronous, so there is no path back into this mutex from inside libusb and
   * no inversion to deadlock on. The bounded wait in uvc_stop_status_xfer() must
   * therefore drop the mutex around its sleep -- holding it would block the very
   * callback it waits for.
   *
   * Initialized in uvc_open_internal(), destroyed in uvc_free_devh(). */
  pthread_mutex_t status_mutex;
  /** Non-zero while `status_xfer` is submitted to libusb -- set by
   * uvc_open_internal() before it submits, cleared by _uvc_status_callback() on
   * the libusb event thread once the transfer is no longer resubmitted. Polled
   * by uvc_stop_status_xfer() on the closing thread. Guarded by status_mutex on
   * every access; see there for why `volatile` was not enough.
   *
   * It is set BEFORE the submit, not after: the callback can run to completion on
   * the event thread while the opening thread is still between the two
   * statements, and a post-submit store would then overwrite the callback's clear
   * with a stale 1, permanently marking a transfer libusb no longer owns as
   * in-flight. */
  uint8_t status_xfer_submitted;
  /** Set by uvc_stop_status_xfer() BEFORE it cancels `status_xfer`: tells
   * _uvc_status_callback() to stop resubmitting. Without it the callback keeps
   * re-arming the interrupt URB on the VideoControl interface AFTER uvc_close()
   * released that interface and reattached the kernel driver -- usbfs then logs
   * "did not claim interface N before use" and steals the interface back, so the
   * final libusb_close() leaves it bound to no driver at all. Never cleared.
   * Guarded by status_mutex on every access. */
  uint8_t status_xfer_stopping;
  /** Set when uvc_stop_status_xfer()'s bounded wait expired with `status_xfer`
   * still owned by libusb. Quarantines the handle for the same reason
   * has_quarantined_stream does: a late callback dereferences devh. */
  uint8_t has_quarantined_status_xfer;
  /** Out-of-process backstop that hands the kernel driver back for every
   * interface this handle still holds, however the process ends -- including
   * the exits that run no user code at all (SIGKILL, SIGSEGV, the watchdog's
   * SIGABRT) and the quarantine paths above, which deliberately never release
   * anything. Created lazily by uvc_claim_if() and released by uvc_free_devh(),
   * i.e. only where the handle is genuinely freed. NULL when the guard is
   * compiled out or could not be started; every entry point tolerates that.
   * See reattach_guard.h. */
  uvc_reattach_guard_t *reattach_guard;
};

/** Context within which we communicate with devices */
struct uvc_context {
  /** Underlying context for USB communication */
  struct libusb_context *usb_ctx;
  /** True iff libuvc initialized the underlying USB context */
  uint8_t own_usb_ctx;
  /** List of open devices in this context */
  uvc_device_handle_t *open_devices;
  pthread_t handler_thread;
  int kill_handler_thread;
  /** Set once uvc_close() had to QUARANTINE a device handle on this context
   * because uvc_stream_stop()'s bounded wait timed out with libusb transfers
   * still outstanding (a dead/unplugged device whose cancelled transfers never
   * completed). The quarantine branch of uvc_close() unlinks the device but
   * deliberately does NOT set kill_handler_thread / pthread_join the event
   * handler (that would risk the unbounded hang the bounded wait removed), so
   * _uvc_handle_events() may still be looping on usb_ctx. While this flag is
   * set, uvc_exit() must NOT libusb_exit(usb_ctx) or free(ctx): the running
   * handler thread dereferences both every iteration -- freeing them is a
   * use-after-free / race on the libusb context itself. It also gates
   * uvc_open_internal() so a reconnect that reopens a device on this context
   * never spawns a SECOND handler thread (the first, from the quarantined
   * close, was never killed and keeps servicing the whole libusb context,
   * including any newly-opened device). Intentionally leak ctx + usb_ctx,
   * mirroring the strmh/devh quarantine leaks. Never cleared. */
  uint8_t has_quarantined_device;
};

uvc_error_t uvc_query_stream_ctrl(
    uvc_device_handle_t *devh,
    uvc_stream_ctrl_t *ctrl,
    uint8_t probe,
    enum uvc_req_code req);

void uvc_start_handler_thread(uvc_context_t *ctx);
uvc_error_t uvc_claim_if(uvc_device_handle_t *devh, int idx);
uvc_error_t uvc_release_if(uvc_device_handle_t *devh, int idx);

#endif // !def(LIBUVC_INTERNAL_H)
/** @endcond */

