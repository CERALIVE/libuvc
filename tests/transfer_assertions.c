#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void LIBUSB_CALL _uvc_stream_callback(struct libusb_transfer *transfer);
void __real_libusb_free_transfer(struct libusb_transfer *transfer);
void __real_free(void *pointer);

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
    return EXIT_FAILURE; \
  } \
} while (0)

static struct libusb_transfer *expected_transfer;
static void *expected_buffer;
static int submit_result;
static int submit_calls;
static int transfer_free_calls;
static int buffer_free_calls;
static int unexpected_call;
static int observe_free_calls;

int __wrap_libusb_submit_transfer(struct libusb_transfer *transfer) {
  submit_calls++;
  if (transfer != expected_transfer)
    unexpected_call = 1;
  return submit_result;
}

void __wrap_libusb_free_transfer(struct libusb_transfer *transfer) {
  transfer_free_calls++;
  if (transfer != expected_transfer)
    unexpected_call = 1;
  __real_libusb_free_transfer(transfer);
}

void __wrap_free(void *pointer) {
  if (observe_free_calls) {
    if (pointer == expected_buffer)
      buffer_free_calls++;
    else {
      unexpected_call = 1;
      return;
    }
  }
  __real_free(pointer);
}

static void reset_mock(struct libusb_transfer *transfer, void *buffer,
                       int result) {
  expected_transfer = transfer;
  expected_buffer = buffer;
  submit_result = result;
  submit_calls = 0;
  transfer_free_calls = 0;
  buffer_free_calls = 0;
  unexpected_call = 0;
  observe_free_calls = 0;
}

static int run_status(enum libusb_transfer_status status, int running,
                      int result, int expected_submits, int expected_frees) {
  uvc_stream_handle_t stream;
  struct libusb_transfer *transfer;
  unsigned char *buffer;

  memset(&stream, 0, sizeof(stream));
  CHECK(pthread_mutex_init(&stream.cb_mutex, NULL) == 0);
  CHECK(pthread_cond_init(&stream.cb_cond, NULL) == 0);
  transfer = libusb_alloc_transfer(0);
  CHECK(transfer != NULL);
  buffer = malloc(8);
  CHECK(buffer != NULL);
  transfer->user_data = &stream;
  transfer->buffer = buffer;
  transfer->status = status;
  stream.running = (uint8_t) running;
  stream.transfers[3] = transfer;
  reset_mock(transfer, buffer, result);

  observe_free_calls = 1;
  _uvc_stream_callback(transfer);
  observe_free_calls = 0;

  CHECK(submit_calls == expected_submits);
  CHECK(transfer_free_calls == expected_frees);
  CHECK(buffer_free_calls == expected_frees);
  CHECK(unexpected_call == 0);
  CHECK((stream.transfers[3] == NULL) == (expected_frees != 0));
  if (!expected_frees) {
    __real_free(buffer);
    __real_libusb_free_transfer(transfer);
  }
  CHECK(pthread_cond_destroy(&stream.cb_cond) == 0);
  CHECK(pthread_mutex_destroy(&stream.cb_mutex) == 0);
  return EXIT_SUCCESS;
}

static int check_terminal_statuses(void) {
  static const enum libusb_transfer_status statuses[] = {
    LIBUSB_TRANSFER_CANCELLED,
    LIBUSB_TRANSFER_ERROR,
    LIBUSB_TRANSFER_NO_DEVICE
  };
  size_t i;
  for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
    CHECK(run_status(statuses[i], 1, 0, 0, 1) == EXIT_SUCCESS);
  return EXIT_SUCCESS;
}

static int check_retry_success(void) {
  static const enum libusb_transfer_status statuses[] = {
    LIBUSB_TRANSFER_TIMED_OUT,
    LIBUSB_TRANSFER_STALL,
    LIBUSB_TRANSFER_OVERFLOW
  };
  size_t i;
  for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
    CHECK(run_status(statuses[i], 1, 0, 1, 0) == EXIT_SUCCESS);
  return EXIT_SUCCESS;
}

static int check_retry_failure(void) {
  static const enum libusb_transfer_status statuses[] = {
    LIBUSB_TRANSFER_TIMED_OUT,
    LIBUSB_TRANSFER_STALL,
    LIBUSB_TRANSFER_OVERFLOW
  };
  size_t i;
  for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); ++i)
    CHECK(run_status(statuses[i], 1, LIBUSB_ERROR_IO, 1, 1) == EXIT_SUCCESS);
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  CHECK(argc == 3 && strcmp(argv[1], "--case") == 0);
  if (strcmp(argv[2], "terminal_statuses") == 0)
    return check_terminal_statuses();
  if (strcmp(argv[2], "retry_success") == 0)
    return check_retry_success();
  if (strcmp(argv[2], "retry_failure") == 0)
    return check_retry_failure();
  fprintf(stderr, "unknown case: %s\n", argv[2]);
  return EXIT_FAILURE;
}
