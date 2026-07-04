cmake_minimum_required(VERSION 3.28)

project(pjsocky
  VERSION 0.1.0
  DESCRIPTION "Headless SIP audio/video call daemon built on pjproject"
  LANGUAGES C
)

# ##############################################################################
# pjproject sibling checkout location
#
# pjsocky links pjproject as a sibling CMake subdirectory rather than
# vendoring or installing it. The default assumes the conventional layout
# (pjsocky and pjproject checked out next to each other), but every
# developer's machine may differ.
#
# To override without touching anything tracked by git, create a
# "cmake.local" file next to this one (see cmake.local.example) containing,
# e.g.:
#
#   set(PJSOCKY_PJPROJECT_DIR "/home/you/somewhere/pjproject")
#
# cmake.local is listed in .gitignore and must never be committed - it is
# personal, per-checkout configuration, not project configuration.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake.local" OPTIONAL)

if(NOT DEFINED PJSOCKY_PJPROJECT_DIR)
  set(PJSOCKY_PJPROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pjproject")
endif()

get_filename_component(PJSOCKY_PJPROJECT_DIR "${PJSOCKY_PJPROJECT_DIR}" ABSOLUTE)

if(NOT EXISTS "${PJSOCKY_PJPROJECT_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "Could not find a pjproject checkout at:\n"
    "  ${PJSOCKY_PJPROJECT_DIR}\n"
    "Either check out pjproject as a sibling of pjsocky (../pjproject), or "
    "create cmake.local (copy cmake.local.example) and set "
    "PJSOCKY_PJPROJECT_DIR to the correct path."
  )
endif()

message(STATUS "pjsocky: using pjproject at ${PJSOCKY_PJPROJECT_DIR}")

# pjproject's own top-level CMakeLists.txt prints a loud experimental-build
# warning unless this is set. We build against it deliberately, so silence it
# instead of scaring every contributor on every configure.
set(PJ_SKIP_EXPERIMENTAL_NOTICE ON CACHE BOOL "Silence pjproject's experimental-cmake-build notice" FORCE)

# pjproject's cmake/FindFFMPEG.cmake uses an if() operator (IS_READABLE)
# that doesn't exist in CMake, which breaks configure whenever
# PJMEDIA_WITH_VIDEO pulls in PJMEDIA_WITH_FFMPEG (its default). This is a
# pre-existing bug in pjproject's own (experimental) CMake build, not
# something pjsocky should patch in a sibling checkout - see CONTEXT.md's
# "no modifications to pjproject source" rule.
#
# We don't need ffmpeg anyway: pjsocky's codec set is G.711 (ulaw/alaw,
# core pjmedia, no external dependency) and H.264 via OpenH264, which is
# an entirely separate CMake option from ffmpeg's own (unused)
# ffmpeg_vid_codecs.c H.264 path. Disabling ffmpeg here costs nothing.
set(PJMEDIA_WITH_FFMPEG OFF CACHE BOOL "Disabled: pjproject's FindFFMPEG.cmake fails to configure; unneeded for our codec set" FORCE)
set(PJMEDIA_WITH_VIDEODEV_FFMPEG OFF CACHE BOOL "Disabled along with PJMEDIA_WITH_FFMPEG" FORCE)

# ##############################################################################
# Codec selection
#
# pjsocky only needs G.711 (ulaw/alaw) for audio and H.264 for video.
# pjproject's CMake build enables most optional codecs by default, which
# pulls in third-party dependencies (or silently no-ops when they're
# missing) we don't want or need. Pin the set explicitly instead of
# inheriting pjproject's defaults, so "what codecs does pjsocky ship with"
# is answered here, not by whatever happens to be installed on the build
# machine.
set(PJMEDIA_WITH_G711_CODEC ON CACHE BOOL "ulaw/alaw - required" FORCE)
set(PJMEDIA_WITH_OPEN_H264_CODEC ON CACHE BOOL "H.264 via OpenH264 - required" FORCE)

set(PJMEDIA_WITH_L16_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_GSM_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_SPEEX_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_ILBC_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_G722_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_G7221_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_OPENCORE_AMRNB_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_OPENCORE_AMRWB_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_SILK_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_OPUS_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_BCG729_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_LYRA_CODEC OFF CACHE BOOL "Not needed" FORCE)
set(PJMEDIA_WITH_VPX_CODEC OFF CACHE BOOL "Not needed" FORCE)

# ##############################################################################
# Acoustic Echo Cancellation
#
# WebRTC AEC3 only. Like the codecs above, both AEC backends pjproject
# offers are bundled third-party sources (third_party/webrtc_aec3,
# third_party/webrtc) with no system package to find, so - unlike
# ffmpeg/SpeexDSP - there's no broken find-module risk here; this is a
# straightforward feature choice, not a workaround.
set(PJMEDIA_WITH_WEBRTC_AEC3 ON CACHE BOOL "WebRTC AEC3 - required" FORCE)
set(PJMEDIA_WITH_WEBRTC_AEC OFF CACHE BOOL "Legacy WebRTC AEC - superseded by AEC3" FORCE)
set(PJMEDIA_WITH_SPEEX_AEC OFF CACHE BOOL "Not needed - using WebRTC AEC3 instead" FORCE)

# pjproject is not part of pjsocky's own source tree, so it needs an explicit
# binary directory (add_subdirectory requires one for out-of-tree sources).
add_subdirectory("${PJSOCKY_PJPROJECT_DIR}" "${CMAKE_BINARY_DIR}/pjproject")

# pjmedia/include/pjmedia-codec/config_auto.h.cm (pjproject's generated-
# header template) is missing "#cmakedefine01 PJMEDIA_HAS_OPENH264_CODEC"
# entirely, even though pjmedia/CMakeLists.txt sets the CMake variable of
# the same name right before generating that header from the template.
# The result: PJMEDIA_WITH_OPEN_H264_CODEC=ON above has no effect at the
# C level - the macro is simply never defined, so openh264.cpp's own
# "#if defined(PJMEDIA_HAS_OPENH264_CODEC)" guard is false and the whole
# file silently compiles to an empty translation unit (confirmed with
# `nm`: zero symbols in openh264.cpp.o, not even the expected undefined
# WelsCreateSVCEncoder reference). Same class of issue as the
# FindFFMPEG.cmake bug above - worked around here via direct compile
# definitions on pjproject's own targets rather than patching its
# tracked source (see CONTEXT.md's "no modifications to pjproject
# source" rule).
#
# Needed on two targets, not just one: pjmedia-codec (openh264.cpp
# itself) AND pjsua-lib (pjsua_vid.c has its own
# "#if PJMEDIA_HAS_VIDEO && PJMEDIA_HAS_OPENH264_CODEC" guard around the
# call to pjmedia_codec_openh264_vid_init() - without this, the codec
# compiles in but nothing ever calls its init function, and the linker
# drops the whole unreferenced object file from the final binary anyway).
if(PJMEDIA_WITH_OPEN_H264_CODEC)
  if(TARGET pjmedia-codec)
    target_compile_definitions(pjmedia-codec PRIVATE PJMEDIA_HAS_OPENH264_CODEC=1)
  endif()
  if(TARGET pjsua-lib)
    target_compile_definitions(pjsua-lib PRIVATE PJMEDIA_HAS_OPENH264_CODEC=1)
  endif()
endif()

# ##############################################################################
# pjsocky

add_executable(pjsocky
  src/main.c
  src/account.c
  src/call.c
  src/device.c
  src/im.c
  src/proto/framing.c
  src/proto/jsonutil.c
  src/proto/dispatch.c
  src/proto/events.c
  src/proto/server.c
)

target_link_libraries(pjsocky
  PRIVATE
    pjlib
    pjlib-util
    pjnath
    pjmedia
    pjmedia-codec
    pjmedia-audiodev
    pjmedia-videodev
    pjsip
    pjsip-simple
    pjsua-lib
)

# Reported in the "hello" event (docs/PROTOCOL.md "Versioning") as
# daemon_version - derived from the single source of truth (this
# project()'s VERSION) rather than a second hardcoded string in C.
target_compile_definitions(pjsocky PRIVATE PJSOCKY_VERSION="${PROJECT_VERSION}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pjsocky PRIVATE -Wall -Wextra)
endif()
