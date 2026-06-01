cmake_minimum_required(VERSION 3.15)
set(PROJECT_NAME "flutter_webrtc")
project(${PROJECT_NAME} LANGUAGES CXX)

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  cmake_policy(SET CMP0135 NEW)
endif()

# Add the libwebrtc dependency (headers only — we link statically)
include("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/CMakeLists.txt")

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "flutter_webrtc_plugin")

# Статическая линковка webrtc.lib с CoreAudio2 WASAPI ADM.
# LIB_WEBRTC_API_DLL УБРАН — больше не тянем libwebrtc.dll.
add_definitions(-DRTC_DESKTOP_DEVICE)
add_definitions(-DWEBRTC_WIN)
add_definitions(-DWEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE)
# Prevent windows.h from defining min/max macros that conflict with
# std::min/std::max and winsock.h/winsock2.h redefinition errors.
add_definitions(-DNOMINMAX)
add_definitions(-DWIN32_LEAN_AND_MEAN)

add_library(${PLUGIN_NAME} SHARED
  "../common/cpp/src/flutter_common.cc"
  "../common/cpp/src/flutter_data_channel.cc"
  "../common/cpp/src/flutter_data_packet_cryptor.cc"
  "../common/cpp/src/flutter_frame_cryptor.cc"
  "../common/cpp/src/flutter_media_stream.cc"
  "../common/cpp/src/flutter_utf8_sanitize.cc"
  "../common/cpp/src/flutter_peerconnection.cc"
  "../common/cpp/src/flutter_frame_capturer.cc"
  "../common/cpp/src/flutter_video_renderer.cc"
  "../common/cpp/src/flutter_screen_capture.cc"
  "../common/cpp/src/flutter_webrtc.cc"
  "../common/cpp/src/flutter_webrtc_base.cc"
  "flutter_webrtc_plugin.cc"
  "task_runner_windows.cc"
  "audio_session_naming.cc"
  # webrtc-sdk: compile SDK wrapper inside plugin (no separate DLL)
  "../third_party/libwebrtc/webrtc-sdk/src/libwebrtc.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/helper.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_peerconnection_factory_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_peerconnection_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_media_stream_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_audio_device_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_audio_source_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_audio_track_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_audio_processing_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_data_channel_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_dtmf_sender_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_ice_candidate_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_mediaconstraints_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_rtp_capabilities_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_rtp_receiver_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_rtp_sender_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_rtp_transceiver_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_rtp_parameters_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_session_description_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_logging.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_video_device_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_video_source_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_video_track_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_video_frame_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_video_sink_adapter.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_desktop_device_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/rtc_desktop_media_list_impl.cc"
  "../third_party/libwebrtc/webrtc-sdk/src/internal/custom_audio_transport_impl.cc"
)

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/svpng"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/include"
  # webrtc-sdk headers
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/webrtc-sdk"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/webrtc-sdk/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/webrtc-sdk/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/webrtc-sdk/src/base"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/webrtc-sdk/src/internal"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/webrtc-sdk/src/desktop"
  # Libwebrtc full source tree (needed for SDK internal #include: api/, modules/, rtc_base/)
  "C:/PROJECTS/webrtc_build/src"
  "C:/PROJECTS/webrtc_build/src/third_party/abseil-cpp"
)

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/include"
)

# Статическая линковка кастомного webrtc.lib (CoreAudio2 WASAPI ADM)
target_link_libraries(${PLUGIN_NAME} PRIVATE 
  flutter
  flutter_wrapper_plugin
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/lib/win64/webrtc.lib"
  ws2_32
  secur32
  winmm
  dmoguids
  wmcodecdspuuid
  msdmo
  strmiids
)

# No bundled DLL — statically linked
set(flutter_webrtc_bundled_libraries
  ""
  PARENT_SCOPE
)
