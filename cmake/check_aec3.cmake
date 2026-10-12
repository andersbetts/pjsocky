# Fails the pjsocky build when WebRTC AEC3 was asked for and did not make it
# into the binary.
#
# Run with -P after the link (see CMakeLists.txt). pjproject's CMake build has
# compiled AEC3 out without a word before - the define it passes and the one its
# sources test had different names - and the daemon then ran every call on
# pjmedia's echo suppressor. The module's source name is a log string in the
# binary only when that module has code, so its presence is the test. It
# survives stripping, which is why the products' pack check reads the same
# string.
#
# Expects: BIN, the linked executable.

file(STRINGS "${BIN}" aec3 REGEX "echo_webrtc_aec3\\.cpp" LIMIT_COUNT 1)
if(NOT aec3)
    message(FATAL_ERROR
        "${BIN} was built with PJMEDIA_WITH_WEBRTC_AEC3=ON but contains no "
        "AEC3 echo canceller: echo_webrtc_aec3.cpp compiled to nothing. Check "
        "that the PJMEDIA_HAS_WEBRTC_AEC3 workaround in CMakeLists.txt still "
        "matches what pjproject's sources test.")
endif()
