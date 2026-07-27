cmake_minimum_required(VERSION 3.1)
project(libuvc
  VERSION 0.0.7
  LANGUAGES C
)

# Additional search scripts path for libusb-1.0, libjpeg, OpenCV
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

# Xcode and Visual Studio do not using CMAKE_BUILD_TYPE cache variable
# so we use Release build type only with single configuration generators.
if (NOT CMAKE_CONFIGURATION_TYPES)
  if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected, default to Release")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
  endif()
endif()

if(NOT CMAKE_BUILD_TARGET)
    message(STATUS "No target type selected, default to both shared and static library")
    set(CMAKE_BUILD_TARGET "Both" CACHE STRING "" FORCE)
endif()

option(BUILD_EXAMPLE "Build example program" ON)
option(BUILD_TEST "Build test program" OFF)
option(BUILD_TESTING "Build hardware-independent regression tests" OFF)
include(CTest)
option(ENABLE_UVC_DEBUGGING "Enable UVC debugging" OFF)
option(LIBUVC_AUTO_DETACH_KERNEL_DRIVER
  "Auto-detach the kernel driver (e.g. uvcvideo) when claiming UVC interfaces (CeraLive)"
  ON)

set(libuvc_DESCRIPTION "A cross-platform library for USB video devices")
set(libuvc_URL "https://github.com/libuvc/libuvc")

set(SOURCES 
  src/ctrl.c
  src/ctrl-gen.c
  src/device.c
  src/diag.c
  src/frame.c
  src/init.c
  src/stream.c
  src/misc.c
)

find_package(LibUSB)

# JpegPkg name to differ from shipped with CMake
find_package(JpegPkg QUIET)
if(JPEG_FOUND)
  message(STATUS "Building libuvc with JPEG support.")
  set(LIBUVC_HAS_JPEG TRUE)
  list(APPEND SOURCES src/frame-mjpeg.c)
else()
  message(WARNING "JPEG not found. libuvc will not support JPEG decoding.")
endif()

if(UNIX AND NOT APPLE)
  set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
  set(THREADS_PREFER_PTHREAD_FLAG TRUE)
  find_package(Threads REQUIRED)
  set(threads Threads::Threads)
endif()

if(${CMAKE_BUILD_TARGET} MATCHES "Shared")
  set(BUILD_UVC_SHARED TRUE)
elseif(${CMAKE_BUILD_TARGET} MATCHES "Static")
  set(BUILD_UVC_STATIC TRUE)
elseif(${CMAKE_BUILD_TARGET} MATCHES "Both")
  set(BUILD_UVC_SHARED TRUE)
  set(BUILD_UVC_STATIC TRUE)
else()
  message(FATAL_ERROR "Invalid build type ${CMAKE_BUILD_TARGET}" )
endif()

if(BUILD_UVC_SHARED)
  add_library(uvc SHARED ${SOURCES})
  set_target_properties(uvc PROPERTIES
    VERSION ${libuvc_VERSION}
    SOVERSION ${libuvc_VERSION_MAJOR}
    # Exported name of target within namespace LibUVC
    EXPORT_NAME UVCShared
  )
  list(APPEND UVC_TARGETS uvc)
  if(NOT LibUVC_STATIC)  
    add_library(LibUVC::UVC ALIAS uvc)
  endif()
  # Aliases defined here available only if project included 
  # via addsubdirectory
  add_library(LibUVC::UVCShared ALIAS uvc)
endif()

if(BUILD_UVC_STATIC)
  add_library(uvc_static STATIC ${SOURCES})
  set_target_properties(uvc_static PROPERTIES
    OUTPUT_NAME uvc
    # Exported name of target within namespace LibUVC
    EXPORT_NAME UVCStatic
  )
  list(APPEND UVC_TARGETS uvc_static)
  add_library(LibUVC::UVCStatic ALIAS uvc_static)
  if(LibUVC_STATIC)
    add_library(LibUVC::UVC ALIAS uvc_static)
  endif()
endif()

configure_file(include/libuvc/libuvc_config.h.in
  include/libuvc/libuvc_config.h
  @ONLY
)

foreach(target_name IN LISTS UVC_TARGETS)
  target_include_directories(${target_name}
    PUBLIC
      # Different paths for includes for build and install phase supported 
      # via INSTALL_INTERFACE and BUILD_INTERFACE generator expressions.
      $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(${target_name}
    # libusb-1.0 used internally so we link to it privately.
    PRIVATE LibUSB::LibUSB ${threads}
  )
  if(JPEG_FOUND)
    target_link_libraries(${target_name}
      PRIVATE JPEG::JPEG
    )
  endif()
  set_target_properties(${target_name} PROPERTIES
      PUBLIC_HEADER "include/libuvc/libuvc.h;${CMAKE_CURRENT_BINARY_DIR}/include/libuvc/libuvc_config.h"
  )
  if(ENABLE_UVC_DEBUGGING)
    target_compile_definitions(${target_name}
      PRIVATE
        UVC_DEBUGGING
    )
    find_library(
            log-lib
            log
    )
    target_link_libraries(
            ${target_name}
            PRIVATE
            ${log-lib}
    )
  endif()
endforeach()

if(BUILD_TESTING)
  if(NOT TARGET uvc_static)
    message(FATAL_ERROR
      "BUILD_TESTING requires CMAKE_BUILD_TARGET=Static or Both")
  endif()
  if(NOT UNIX OR APPLE OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "The libuvc regression tests require Linux and GNU-compatible --wrap")
  endif()

  add_executable(uvc_descriptor_assertions tests/descriptor_assertions.c)
  add_executable(uvc_negotiation_assertions tests/negotiation_assertions.c)
  add_executable(uvc_transfer_assertions tests/transfer_assertions.c)
  add_executable(uvc_teardown_assertions tests/teardown_assertions.c)

  foreach(test_target
      uvc_descriptor_assertions
      uvc_negotiation_assertions
      uvc_transfer_assertions
      uvc_teardown_assertions)
    target_link_libraries(${test_target}
      PRIVATE uvc_static LibUSB::LibUSB ${threads})
    if(JPEG_FOUND)
      target_link_libraries(${test_target} PRIVATE JPEG::JPEG)
    endif()
  endforeach()

  set_property(TARGET uvc_descriptor_assertions APPEND_STRING
    PROPERTY LINK_FLAGS " -Wl,--wrap=libusb_get_device_descriptor")

  set_property(TARGET uvc_negotiation_assertions APPEND_STRING
    PROPERTY LINK_FLAGS " -Wl,--wrap=libusb_control_transfer -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/02-link.map")
  set_property(TARGET uvc_transfer_assertions APPEND_STRING
    PROPERTY LINK_FLAGS " -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_free_transfer -Wl,--wrap=free")
  set_property(TARGET uvc_teardown_assertions APPEND_STRING
    PROPERTY LINK_FLAGS " -Wl,--wrap=libusb_submit_transfer -Wl,--wrap=libusb_cancel_transfer -Wl,--wrap=libusb_release_interface -Wl,--wrap=libusb_attach_kernel_driver -Wl,--wrap=libusb_set_interface_alt_setting -Wl,--wrap=libusb_close -Wl,--wrap=libusb_unref_device -Wl,--wrap=nanosleep")

  foreach(case_name
      h264
      h265
      truncated_format
      truncated_frame
      degenerate_h26x
      scanner_vs_oversized
      scanner_vs_zero
      scanner_vs_header_short
      scanner_vc_oversized
      scanner_vc_zero
      scanner_vc_header_short)
    add_test(NAME libuvc.descriptor.${case_name}
      COMMAND uvc_descriptor_assertions --case ${case_name})
  endforeach()
  set_tests_properties(
    libuvc.descriptor.scanner_vs_zero
    libuvc.descriptor.scanner_vc_zero
    PROPERTIES TIMEOUT 2)
  foreach(case_name h264 h265 near_match probe_set_error probe_get_error)
    add_test(NAME libuvc.negotiation.${case_name}
      COMMAND uvc_negotiation_assertions --case ${case_name})
  endforeach()
  foreach(case_name terminal_statuses retry_success retry_failure)
    add_test(NAME libuvc.transfer.${case_name}
      COMMAND uvc_transfer_assertions --case ${case_name})
  endforeach()
  foreach(case_name
      status_xfer_stops_before_control_release
      every_claimed_interface_released_control_last
      no_status_endpoint_unchanged
      undeliverable_status_xfer_quarantines)
    add_test(NAME libuvc.teardown.${case_name}
      COMMAND uvc_teardown_assertions --case ${case_name})
  endforeach()
  set_tests_properties(
    libuvc.teardown.undeliverable_status_xfer_quarantines
    PROPERTIES TIMEOUT 5)
endif()

if(BUILD_EXAMPLE)
  add_executable(example src/example.c)
  find_package(Threads)
  target_link_libraries(example
    PRIVATE
      LibUVC::UVC
      Threads::Threads
  )
endif()

if(BUILD_TEST)
  # OpenCV defines targets with transitive dependencies not with namespaces but using opencv_ prefix. 
  # This targets provide necessary include directories and linked flags.
  find_package(OpenCVPkg REQUIRED
    COMPONENTS
      opencv_core
      opencv_highgui
  )

  add_executable(uvc_test src/test.c)
  target_link_libraries(uvc_test
    PRIVATE
      LibUVC::UVC
      opencv_core
      opencv_highgui
  )
endif()


include(GNUInstallDirs)
set(CMAKE_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/libuvc)

install(
  TARGETS ${UVC_TARGETS}
  EXPORT libuvcTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libuvc
)

install(EXPORT libuvcTargets
  FILE libuvcTargets.cmake
  NAMESPACE LibUVC::
  DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
)

install(FILES 
    cmake/FindLibUSB.cmake
    cmake/FindJpegPkg.cmake
  DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(libuvcConfigVersion.cmake
  COMPATIBILITY AnyNewerVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/libuvcConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
)

if(JPEG_FOUND)
  # If we have used JPEG library we need to 
  # add linker flag for it in config file for pkgconfig
  set(PKGCONFIG_JPEG_LDFLAG "-ljpeg")
endif()

configure_file(libuvc.pc.in
  ${PROJECT_BINARY_DIR}/libuvc.pc
  @ONLY
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/libuvc.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

install(FILES libuvcConfig.cmake
  DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
)

