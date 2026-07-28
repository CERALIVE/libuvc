/** @file reattach_guard.c
 * @brief Out-of-process kernel-driver reattach. See reattach_guard.h for why
 * this cannot be done inside the process that needs it.
 */
/* pipe2() and MAP_ANONYMOUS are glibc extensions to the POSIX base profile. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "libuvc/libuvc_config.h"
#include "libuvc/reattach_guard.h"

#include <stdlib.h>

#if LIBUVC_REATTACH_GUARD && defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/** Marks a mapping this library initialized, so a helper never acts on one it
 * cannot account for. */
#define UVC_REATTACH_MAGIC 0x55564347u /* 'UVCG' */

/** The helper wakes while the dying process's descriptors are still being torn
 * down, and the usbfs one is not necessarily among those already closed. Until
 * it is, the kernel still sees usbfs holding the interface and answers EBUSY.
 * The budget only has to outlast one exit_files() pass. */
#define UVC_REATTACH_RETRY_MS 25
#define UVC_REATTACH_MAX_ATTEMPTS 80

/** Enough to walk a normal descriptor table without turning a huge RLIMIT_NOFILE
 * into a million-syscall loop in a process that is trying to be quick. */
#define UVC_REATTACH_FD_SCAN_CAP 65536

struct uvc_reattach_guard {
  /** Shared with the helper; outlives this process's mapping of it. */
  struct uvc_reattach_record *record;
  /** Write end of the helper's pipe. Closing it -- by any means, including the
   * kernel closing it on process death -- is the wakeup. */
  int notify_fd;
};

static uint16_t read_le16(const uint8_t *bytes) {
  return (uint16_t) ((uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8));
}

/* snprintf() is not async-signal-safe and this runs after fork() in a process
 * that may have been holding the stdio locks. */
static void write_three_digits(char *out, unsigned int value) {
  out[0] = (char) ('0' + (value / 100) % 10);
  out[1] = (char) ('0' + (value / 10) % 10);
  out[2] = (char) ('0' + value % 10);
}

static int default_open_device(const struct uvc_reattach_record *record) {
  char path[] = "/dev/bus/usb/000/000";
  uint8_t descriptor[18];
  ssize_t got;
  int fd;

  write_three_digits(path + 13, record->busnum);
  write_three_digits(path + 17, record->devnum);

  fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -1;

  /* A usbfs node reads back its 18-byte device descriptor first. Bus addresses
   * are reused, so a helper that wakes late could otherwise re-probe an
   * interface of a device this process never touched. */
  got = read(fd, descriptor, sizeof(descriptor));
  if (got != (ssize_t) sizeof(descriptor)
      || read_le16(descriptor + 8) != record->id_vendor
      || read_le16(descriptor + 10) != record->id_product
      || read_le16(descriptor + 12) != record->bcd_device) {
    close(fd);
    return -1;
  }

  return fd;
}

/* Byte for byte what libusb_attach_kernel_driver() issues -- the call the dying
 * process never got to make. */
static int default_rebind(int fd, uint8_t interface_number) {
  struct usbdevfs_ioctl command;

  command.ifno = (int) interface_number;
  command.ioctl_code = USBDEVFS_CONNECT;
  command.data = NULL;

  if (ioctl(fd, USBDEVFS_IOCTL, &command) < 0)
    return -errno;

  return 0;
}

static void default_close_device(int fd) {
  close(fd);
}

static const uvc_reattach_backend_t default_backend = {
  default_open_device,
  default_rebind,
  default_close_device
};

static const uvc_reattach_backend_t *active_backend = &default_backend;

void uvc_reattach_guard_set_backend(const uvc_reattach_backend_t *backend) {
  active_backend = backend ? backend : &default_backend;
}

const uvc_reattach_backend_t *uvc_reattach_default_backend(void) {
  return &default_backend;
}

static void reattach_armed_interfaces(const struct uvc_reattach_record *record) {
  const uvc_reattach_backend_t *backend = active_backend;
  uint32_t armed;
  int interface_number;
  int fd;

  if (record->magic != UVC_REATTACH_MAGIC)
    return;

  armed = record->armed;
  if (armed == 0)
    return;

  fd = backend->open_device(record);
  if (fd < 0)
    return;

  for (interface_number = 0;
       interface_number < UVC_REATTACH_MAX_INTERFACES;
       interface_number++) {
    int attempt;

    if ((armed & (1u << interface_number)) == 0)
      continue;

    for (attempt = 0; attempt < UVC_REATTACH_MAX_ATTEMPTS; attempt++) {
      if (backend->rebind(fd, (uint8_t) interface_number) != -EBUSY)
        break;
      /* poll(), not nanosleep(): the teardown regression suite --wraps
       * nanosleep to drive its synthetic event thread, and a forked helper must
       * not fall into it. */
      poll(NULL, 0, UVC_REATTACH_RETRY_MS);
    }
  }

  backend->close_device(fd);
}

static int fd_scan_limit(void) {
  long limit = sysconf(_SC_OPEN_MAX);

  if (limit < 3)
    return 3;
  if (limit > UVC_REATTACH_FD_SCAN_CAP)
    return UVC_REATTACH_FD_SCAN_CAP;
  return (int) limit;
}

/* Everything the fork copied belongs to the host process. Holding any of it
 * would keep a socket or pipe alive past the host's death and its peers would
 * never see EOF -- a worse bug than the one being fixed. This also closes the
 * helper's own copy of the pipe's WRITE end, without which the read below could
 * never see EOF. */
static void close_inherited_fds(int keep_fd, int scan_limit) {
  int fd;

  for (fd = 0; fd < scan_limit; fd++) {
    if (fd != keep_fd)
      close(fd);
  }
}

static void helper_main(int notify_fd, int scan_limit,
                        const struct uvc_reattach_record *record) {
  char drain[64];

  /* A `kill -9` aimed at the whole process group must not take the repair with
   * it. */
  setsid();
  close_inherited_fds(notify_fd, scan_limit);

  for (;;) {
    ssize_t got = read(notify_fd, drain, sizeof(drain));

    if (got == 0)
      break; /* EOF: every write end is gone, so the arming process is too. */
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0)
      break; /* Should not happen; repairing is the safe direction anyway. */
    /* Nothing ever writes to this pipe, so a byte is noise. Keep waiting. */
  }

  reattach_armed_interfaces(record);
  _exit(0);
}

uvc_reattach_guard_t *uvc_reattach_guard_create(uint8_t busnum, uint8_t devnum,
                                                uint16_t id_vendor,
                                                uint16_t id_product,
                                                uint16_t bcd_device) {
  uvc_reattach_guard_t *guard;
  struct uvc_reattach_record *record;
  int notify[2];
  int scan_limit;
  pid_t intermediate;

  guard = calloc(1, sizeof(*guard));
  if (guard == NULL)
    return NULL;

  /* MAP_SHARED so the helper's mapping refers to the same object: this
   * process's munmap() at destroy time does not pull it out from under it. */
  record = mmap(NULL, sizeof(*record), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (record == MAP_FAILED) {
    free(guard);
    return NULL;
  }

  record->magic = UVC_REATTACH_MAGIC;
  record->armed = 0;
  record->busnum = busnum;
  record->devnum = devnum;
  record->id_vendor = id_vendor;
  record->id_product = id_product;
  record->bcd_device = bcd_device;

  /* Close-on-exec matters in both directions: an exec in the host must wake the
   * helper, because the libuvc state that would have released the interfaces
   * died with the old image; and a helper process the host fork+execs must not
   * inherit the write end and hold the pipe open past the host's death. */
  if (pipe2(notify, O_CLOEXEC) != 0) {
    munmap(record, sizeof(*record));
    free(guard);
    return NULL;
  }

  scan_limit = fd_scan_limit();

  intermediate = fork();
  if (intermediate == 0) {
    /* Double fork: the helper is reparented to init, so the host is never
     * handed a SIGCHLD for a process it did not create and never has a zombie
     * it does not know to reap. */
    if (fork() == 0)
      helper_main(notify[0], scan_limit, record); /* does not return */
    _exit(0);
  }

  close(notify[0]);

  if (intermediate < 0) {
    close(notify[1]);
    munmap(record, sizeof(*record));
    free(guard);
    return NULL;
  }

  while (waitpid(intermediate, NULL, 0) < 0 && errno == EINTR)
    continue;

  guard->record = record;
  guard->notify_fd = notify[1];
  return guard;
}

void uvc_reattach_guard_arm(uvc_reattach_guard_t *guard, int interface_number) {
  if (guard == NULL || interface_number < 0
      || interface_number >= UVC_REATTACH_MAX_INTERFACES)
    return;

  guard->record->armed |= 1u << interface_number;
}

void uvc_reattach_guard_disarm(uvc_reattach_guard_t *guard,
                               int interface_number) {
  if (guard == NULL || interface_number < 0
      || interface_number >= UVC_REATTACH_MAX_INTERFACES)
    return;

  guard->record->armed &= ~(1u << interface_number);
}

uint32_t uvc_reattach_guard_armed_mask(const uvc_reattach_guard_t *guard) {
  if (guard == NULL)
    return 0;

  return guard->record->armed;
}

void uvc_reattach_guard_destroy(uvc_reattach_guard_t *guard) {
  if (guard == NULL)
    return;

  /* Closing the write end IS the wakeup. Anything still armed at this point is
   * an interface the teardown failed to hand back, which is exactly what the
   * helper is for -- so this is deliberately not conditional on the mask. */
  close(guard->notify_fd);
  munmap(guard->record, sizeof(*guard->record));
  free(guard);
}

#else /* !LIBUVC_REATTACH_GUARD || !defined(__linux__) */

uvc_reattach_guard_t *uvc_reattach_guard_create(uint8_t busnum, uint8_t devnum,
                                                uint16_t id_vendor,
                                                uint16_t id_product,
                                                uint16_t bcd_device) {
  (void) busnum;
  (void) devnum;
  (void) id_vendor;
  (void) id_product;
  (void) bcd_device;
  return NULL;
}

void uvc_reattach_guard_arm(uvc_reattach_guard_t *guard, int interface_number) {
  (void) guard;
  (void) interface_number;
}

void uvc_reattach_guard_disarm(uvc_reattach_guard_t *guard,
                               int interface_number) {
  (void) guard;
  (void) interface_number;
}

uint32_t uvc_reattach_guard_armed_mask(const uvc_reattach_guard_t *guard) {
  (void) guard;
  return 0;
}

void uvc_reattach_guard_destroy(uvc_reattach_guard_t *guard) {
  (void) guard;
}

void uvc_reattach_guard_set_backend(const uvc_reattach_backend_t *backend) {
  (void) backend;
}

const uvc_reattach_backend_t *uvc_reattach_default_backend(void) {
  return NULL;
}

#endif /* LIBUVC_REATTACH_GUARD && defined(__linux__) */
