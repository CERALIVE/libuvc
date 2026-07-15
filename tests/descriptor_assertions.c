#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uvc_error_t uvc_parse_vs_frame_format(uvc_streaming_interface_t *,
                                      const unsigned char *, size_t);
uvc_error_t uvc_parse_vs_frame_frame(uvc_streaming_interface_t *,
                                     const unsigned char *, size_t);
uvc_error_t uvc_scan_control(uvc_device_handle_t *, uvc_device_info_t *);
uvc_error_t uvc_scan_streaming(uvc_device_t *, uvc_device_info_t *, int);

int __wrap_libusb_get_device_descriptor(
    libusb_device *device, struct libusb_device_descriptor *descriptor) {
  (void) device;
  (void) descriptor;
  return LIBUSB_ERROR_NO_DEVICE;
}

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
            __FILE__, __LINE__, #expression); \
    return EXIT_FAILURE; \
  } \
} while (0)

static const unsigned char h264_guid[16] = {
  'H', '2', '6', '4', 0x00, 0x00, 0x10, 0x00,
  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};
static const unsigned char h265_guid[16] = {
  'H', '2', '6', '5', 0x00, 0x00, 0x10, 0x00,
  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};

static void make_format(unsigned char block[28], const unsigned char guid[16]) {
  memset(block, 0, 28);
  block[0] = 28;
  block[1] = 0x24;
  block[2] = UVC_VS_FORMAT_FRAME_BASED;
  block[3] = 7;
  block[4] = 1;
  memcpy(block + 5, guid, 16);
  block[21] = 0;
  block[22] = 1;
}

static void put_u32(unsigned char *destination, uint32_t value) {
  destination[0] = (unsigned char) value;
  destination[1] = (unsigned char) (value >> 8);
  destination[2] = (unsigned char) (value >> 16);
  destination[3] = (unsigned char) (value >> 24);
}

static void make_frame(unsigned char block[30], uint32_t buffer_size,
                       uint32_t default_interval) {
  memset(block, 0, 30);
  block[0] = 30;
  block[1] = 0x24;
  block[2] = UVC_VS_FRAME_FRAME_BASED;
  block[3] = 3;
  block[5] = 0x80;
  block[6] = 0x07;
  block[7] = 0x38;
  block[8] = 0x04;
  put_u32(block + 17, default_interval);
  block[21] = 1;
  put_u32(block + 22, buffer_size);
  put_u32(block + 26, 333333);
}

static void release_stream(uvc_streaming_interface_t *stream_if) {
  uvc_format_desc_t *format = stream_if->format_descs;
  if (format) {
    uvc_frame_desc_t *frame = format->frame_descs;
    if (frame) {
      free(frame->intervals);
      free(frame);
    }
    free(format);
  }
  stream_if->format_descs = NULL;
}

static uvc_error_t scan_streaming_extra(const unsigned char *extra,
                                        size_t extra_length) {
  unsigned char *owned_extra = malloc(extra_length);
  struct libusb_interface_descriptor altsetting = {0};
  struct libusb_interface interface = {0};
  struct libusb_config_descriptor config = {0};
  uvc_device_info_t info = {0};
  uvc_device_t device = {0};
  uvc_error_t result;

  if (!owned_extra)
    return UVC_ERROR_NO_MEM;
  memcpy(owned_extra, extra, extra_length);
  altsetting.extra = owned_extra;
  altsetting.extra_length = (int) extra_length;
  interface.altsetting = &altsetting;
  interface.num_altsetting = 1;
  config.interface = &interface;
  config.bNumInterfaces = 1;
  info.config = &config;

  result = uvc_scan_streaming(&device, &info, 0);
  if (info.stream_ifs) {
    release_stream(info.stream_ifs);
    free(info.stream_ifs);
  }
  free(owned_extra);
  return result;
}

static uvc_error_t scan_control_extra(const unsigned char *extra,
                                      size_t extra_length) {
  unsigned char *owned_extra = malloc(extra_length);
  struct libusb_interface_descriptor altsetting = {0};
  struct libusb_interface interface = {0};
  struct libusb_config_descriptor config = {0};
  uvc_device_info_t info = {0};
  uvc_device_t device = {0};
  uvc_device_handle_t device_handle = {0};
  uvc_error_t result;

  if (!owned_extra)
    return UVC_ERROR_NO_MEM;
  memcpy(owned_extra, extra, extra_length);
  altsetting.bInterfaceClass = 14;
  altsetting.bInterfaceSubClass = 1;
  altsetting.extra = owned_extra;
  altsetting.extra_length = (int) extra_length;
  interface.altsetting = &altsetting;
  interface.num_altsetting = 1;
  config.interface = &interface;
  config.bNumInterfaces = 1;
  info.config = &config;
  device_handle.dev = &device;

  result = uvc_scan_control(&device_handle, &info);
  free(owned_extra);
  return result;
}

static int check_codec(const unsigned char guid[16]) {
  unsigned char block[28];
  uvc_streaming_interface_t stream_if = {0};
  uvc_format_desc_t *format;

  make_format(block, guid);
  CHECK(uvc_parse_vs_frame_format(&stream_if, block, sizeof(block)) == UVC_SUCCESS);
  format = stream_if.format_descs;
  CHECK(format != NULL);
  CHECK(format->bDescriptorSubtype == UVC_VS_FORMAT_FRAME_BASED);
  CHECK(format->bFormatIndex == 7);
  CHECK(format->bNumFrameDescriptors == 1);
  CHECK(memcmp(format->guidFormat, guid, 16) == 0);
  release_stream(&stream_if);
  return EXIT_SUCCESS;
}

static int check_truncated_format(void) {
  unsigned char block[28];
  uvc_streaming_interface_t stream_if = {0};
  uvc_error_t result;

  make_format(block, h264_guid);
  result = uvc_parse_vs_frame_format(&stream_if, block, 27);
  if (result != UVC_ERROR_INVALID_DEVICE)
    fprintf(stderr, "observed=%d required=%d\n", result,
            UVC_ERROR_INVALID_DEVICE);
  CHECK(result == UVC_ERROR_INVALID_DEVICE);
  CHECK(stream_if.format_descs == NULL);
  return EXIT_SUCCESS;
}

static int check_truncated_frame(void) {
  unsigned char format_block[28];
  unsigned char frame_block[30];
  uvc_streaming_interface_t stream_if = {0};
  uvc_streaming_interface_t unordered = {0};

  make_format(format_block, h264_guid);
  make_frame(frame_block, 4147200, 333333);
  CHECK(uvc_parse_vs_frame_format(&stream_if, format_block,
                                  sizeof(format_block)) == UVC_SUCCESS);
  {
    uvc_error_t result = uvc_parse_vs_frame_frame(&stream_if, frame_block, 29);
    if (result != UVC_ERROR_INVALID_DEVICE)
      fprintf(stderr, "observed=%d required=%d\n", result,
              UVC_ERROR_INVALID_DEVICE);
    CHECK(result == UVC_ERROR_INVALID_DEVICE);
  }
  CHECK(stream_if.format_descs->frame_descs == NULL);
  CHECK(uvc_parse_vs_frame_frame(&unordered, frame_block,
                                 sizeof(frame_block)) ==
        UVC_ERROR_INVALID_DEVICE);
  release_stream(&stream_if);
  return EXIT_SUCCESS;
}

static int check_degenerate(void) {
  unsigned char format_block[28];
  unsigned char frame_block[30];
  uvc_streaming_interface_t stream_if = {0};
  uvc_frame_desc_t *frame;

  make_format(format_block, h265_guid);
  make_frame(frame_block, 0, 0);
  CHECK(uvc_parse_vs_frame_format(&stream_if, format_block,
                                  sizeof(format_block)) == UVC_SUCCESS);
  CHECK(uvc_parse_vs_frame_frame(&stream_if, frame_block,
                                 sizeof(frame_block)) == UVC_SUCCESS);
  frame = stream_if.format_descs->frame_descs;
  CHECK(frame != NULL);
  CHECK(frame->wWidth == 1920 && frame->wHeight == 1080);
  CHECK(frame->dwMaxVideoFrameBufferSize == 1920U * 1080U * 2U);
  CHECK(frame->dwDefaultFrameInterval == 333333);
  CHECK(frame->intervals[0] == 333333 && frame->intervals[1] == 0);
  release_stream(&stream_if);
  return EXIT_SUCCESS;
}

static int check_scanner_vs_oversized(void) {
  const unsigned char extra[3] = {
    28, 0x24, UVC_VS_FORMAT_FRAME_BASED
  };

  /* Given a three-byte VS descriptor that declares 28 bytes. */
  /* When the production VideoStreaming scanner walks the descriptor. */
  uvc_error_t result = scan_streaming_extra(extra, sizeof(extra));
  /* Then it rejects the descriptor before parser dispatch. */
  CHECK(result == UVC_ERROR_INVALID_DEVICE);
  return EXIT_SUCCESS;
}

static int check_scanner_vs_zero(void) {
  const unsigned char extra[3] = {0, 0x24, UVC_VS_OUTPUT_HEADER};

  /* Given a complete VS header whose declared length is zero. */
  /* When the production VideoStreaming scanner walks the descriptor. */
  uvc_error_t result = scan_streaming_extra(extra, sizeof(extra));
  /* Then it rejects the descriptor without stalling. */
  CHECK(result == UVC_ERROR_INVALID_DEVICE);
  return EXIT_SUCCESS;
}

static int check_scanner_vs_header_short(void) {
  const unsigned char extra[2] = {2, 0x24};

  /* Given a VS extra block shorter than the three-byte header. */
  /* When the production VideoStreaming scanner walks the descriptor. */
  uvc_error_t result = scan_streaming_extra(extra, sizeof(extra));
  /* Then it rejects the incomplete header. */
  CHECK(result == UVC_ERROR_INVALID_DEVICE);
  return EXIT_SUCCESS;
}

static int check_scanner_vc_oversized(void) {
  const unsigned char extra[3] = {
    28, 0x24, UVC_VC_INPUT_TERMINAL
  };

  /* Given a three-byte VC descriptor that declares 28 bytes. */
  /* When the production VideoControl scanner walks the descriptor. */
  uvc_error_t result = scan_control_extra(extra, sizeof(extra));
  /* Then it rejects the descriptor before parser dispatch. */
  CHECK(result == UVC_ERROR_INVALID_DEVICE);
  return EXIT_SUCCESS;
}

static int check_scanner_vc_zero(void) {
  const unsigned char extra[3] = {0, 0x24, UVC_VC_OUTPUT_TERMINAL};

  /* Given a complete VC header whose declared length is zero. */
  /* When the production VideoControl scanner walks the descriptor. */
  uvc_error_t result = scan_control_extra(extra, sizeof(extra));
  /* Then it rejects the descriptor without stalling. */
  CHECK(result == UVC_ERROR_INVALID_DEVICE);
  return EXIT_SUCCESS;
}

static int check_scanner_vc_header_short(void) {
  const unsigned char extra[2] = {2, 0x24};

  /* Given a VC extra block shorter than the three-byte header. */
  /* When the production VideoControl scanner walks the descriptor. */
  uvc_error_t result = scan_control_extra(extra, sizeof(extra));
  /* Then it rejects the incomplete header. */
  CHECK(result == UVC_ERROR_INVALID_DEVICE);
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  CHECK(argc == 3 && strcmp(argv[1], "--case") == 0);
  if (strcmp(argv[2], "h264") == 0)
    return check_codec(h264_guid);
  if (strcmp(argv[2], "h265") == 0)
    return check_codec(h265_guid);
  if (strcmp(argv[2], "truncated_format") == 0)
    return check_truncated_format();
  if (strcmp(argv[2], "truncated_frame") == 0)
    return check_truncated_frame();
  if (strcmp(argv[2], "degenerate_h26x") == 0)
    return check_degenerate();
  if (strcmp(argv[2], "scanner_vs_oversized") == 0)
    return check_scanner_vs_oversized();
  if (strcmp(argv[2], "scanner_vs_zero") == 0)
    return check_scanner_vs_zero();
  if (strcmp(argv[2], "scanner_vs_header_short") == 0)
    return check_scanner_vs_header_short();
  if (strcmp(argv[2], "scanner_vc_oversized") == 0)
    return check_scanner_vc_oversized();
  if (strcmp(argv[2], "scanner_vc_zero") == 0)
    return check_scanner_vc_zero();
  if (strcmp(argv[2], "scanner_vc_header_short") == 0)
    return check_scanner_vc_header_short();
  fprintf(stderr, "unknown case: %s\n", argv[2]);
  return EXIT_FAILURE;
}
