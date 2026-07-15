#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
    return EXIT_FAILURE; \
  } \
} while (0)

enum mock_mode {
  MOCK_SUCCESS,
  MOCK_SET_ERROR,
  MOCK_GET_ERROR
};

static enum mock_mode mode;
static int call_count;
static int unexpected_call;
static uint8_t last_format;
static uint8_t last_frame;
static unsigned char last_interval[4];

int __wrap_libusb_control_transfer(libusb_device_handle *device_handle,
    uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
    unsigned char *data, uint16_t length, unsigned int timeout) {
  (void) device_handle;
  (void) timeout;
  call_count++;
  if (value != (UVC_VS_PROBE_CONTROL << 8) || index != 2 ||
      (length != 26 && length != 34)) {
    unexpected_call = 1;
    return LIBUSB_ERROR_INVALID_PARAM;
  }
  if (request == UVC_SET_CUR) {
    if (request_type != 0x21) {
      unexpected_call = 1;
      return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (mode == MOCK_SET_ERROR)
      return LIBUSB_ERROR_PIPE;
    last_format = data[2];
    last_frame = data[3];
    memcpy(last_interval, data + 4, sizeof(last_interval));
    return length;
  }
  if (request_type != 0xA1 ||
      (request != UVC_GET_MAX && request != UVC_GET_CUR)) {
    unexpected_call = 1;
    return LIBUSB_ERROR_INVALID_PARAM;
  }
  if (request == UVC_GET_CUR && mode == MOCK_GET_ERROR)
    return LIBUSB_ERROR_TIMEOUT;
  if (request == UVC_GET_CUR || request == UVC_GET_MAX) {
    data[2] = request == UVC_GET_CUR ? last_format : 0;
    data[3] = request == UVC_GET_CUR ? last_frame : 0;
    if (request == UVC_GET_CUR)
      memcpy(data + 4, last_interval, sizeof(last_interval));
    return length;
  }
  unexpected_call = 1;
  return LIBUSB_ERROR_INVALID_PARAM;
}

static void reset_mock(enum mock_mode selected_mode) {
  mode = selected_mode;
  call_count = 0;
  unexpected_call = 0;
  last_format = 0;
  last_frame = 0;
  memset(last_interval, 0, sizeof(last_interval));
}

static void make_graph(uvc_device_handle_t *devh, uvc_device_info_t *info,
                       uvc_streaming_interface_t *stream_if,
                       uvc_format_desc_t formats[2],
                       uvc_frame_desc_t frames[2]) {
  static const unsigned char guids[2][16] = {
    {'H','2','6','4',0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71},
    {'H','2','6','5',0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71}
  };
  static uint32_t intervals[2][2] = {{333333, 0}, {333333, 0}};
  int i;

  memset(devh, 0, sizeof(*devh));
  memset(info, 0, sizeof(*info));
  memset(stream_if, 0, sizeof(*stream_if));
  memset(formats, 0, sizeof(uvc_format_desc_t) * 2);
  memset(frames, 0, sizeof(uvc_frame_desc_t) * 2);
  devh->info = info;
  devh->claimed = 1U << 2;
  info->stream_ifs = stream_if;
  stream_if->parent = info;
  stream_if->bInterfaceNumber = 2;
  stream_if->format_descs = formats;
  formats[0].next = &formats[1];
  formats[0].prev = &formats[1];
  formats[1].next = NULL;
  formats[1].prev = &formats[0];
  for (i = 0; i < 2; ++i) {
    formats[i].parent = stream_if;
    formats[i].bFormatIndex = (uint8_t) (4 + i);
    memcpy(formats[i].guidFormat, guids[i], 16);
    formats[i].frame_descs = &frames[i];
    frames[i].parent = &formats[i];
    frames[i].bFrameIndex = 1;
    frames[i].wWidth = 1920;
    frames[i].wHeight = 1080;
    frames[i].intervals = intervals[i];
  }
}

static int check_selection(enum uvc_frame_format format, uint8_t expected_index) {
  uvc_device_handle_t devh;
  uvc_device_info_t info;
  uvc_streaming_interface_t stream_if;
  uvc_format_desc_t formats[2];
  uvc_frame_desc_t frames[2];
  uvc_stream_ctrl_t ctrl = {0};

  make_graph(&devh, &info, &stream_if, formats, frames);
  reset_mock(MOCK_SUCCESS);
  CHECK(uvc_get_stream_ctrl_format_size(&devh, &ctrl, format,
                                        1920, 1080, 30) == UVC_SUCCESS);
  CHECK(ctrl.bInterfaceNumber == 2);
  CHECK(ctrl.bFormatIndex == expected_index);
  CHECK(ctrl.bFrameIndex == 1);
  CHECK(ctrl.dwFrameInterval == 333333);
  CHECK(call_count == 3 && unexpected_call == 0);
  return EXIT_SUCCESS;
}

static int check_near_match(void) {
  uvc_device_handle_t devh;
  uvc_device_info_t info;
  uvc_streaming_interface_t stream_if;
  uvc_format_desc_t formats[2];
  uvc_frame_desc_t frames[2];
  uvc_stream_ctrl_t ctrl = {0};

  make_graph(&devh, &info, &stream_if, formats, frames);
  formats[0].guidFormat[15] ^= 1;
  reset_mock(MOCK_SUCCESS);
  CHECK(uvc_get_stream_ctrl_format_size(&devh, &ctrl, UVC_FRAME_FORMAT_H264,
                                        1920, 1080, 30) ==
        UVC_ERROR_INVALID_MODE);
  CHECK(call_count == 0 && unexpected_call == 0);
  return EXIT_SUCCESS;
}

static int check_probe_error(enum mock_mode selected_mode,
                             uvc_error_t expected) {
  uvc_device_handle_t devh;
  uvc_device_info_t info;
  uvc_stream_ctrl_t ctrl = {0};

  memset(&devh, 0, sizeof(devh));
  memset(&info, 0, sizeof(info));
  devh.info = &info;
  ctrl.bInterfaceNumber = 2;
  ctrl.bFormatIndex = 4;
  ctrl.bFrameIndex = 1;
  reset_mock(selected_mode);
  {
    uvc_error_t result = uvc_probe_stream_ctrl(&devh, &ctrl);
    if (result != expected)
      fprintf(stderr, "observed=%d required=%d\n", result, expected);
    CHECK(result == expected);
  }
  CHECK(unexpected_call == 0);
  CHECK(call_count == (selected_mode == MOCK_SET_ERROR ? 1 : 2));
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  CHECK(argc == 3 && strcmp(argv[1], "--case") == 0);
  if (strcmp(argv[2], "h264") == 0)
    return check_selection(UVC_FRAME_FORMAT_H264, 4);
  if (strcmp(argv[2], "h265") == 0)
    return check_selection(UVC_FRAME_FORMAT_H265, 5);
  if (strcmp(argv[2], "near_match") == 0)
    return check_near_match();
  if (strcmp(argv[2], "probe_set_error") == 0)
    return check_probe_error(MOCK_SET_ERROR, LIBUSB_ERROR_PIPE);
  if (strcmp(argv[2], "probe_get_error") == 0)
    return check_probe_error(MOCK_GET_ERROR, LIBUSB_ERROR_TIMEOUT);
  fprintf(stderr, "unknown case: %s\n", argv[2]);
  return EXIT_FAILURE;
}
