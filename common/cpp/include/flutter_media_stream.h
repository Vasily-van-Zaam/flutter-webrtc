#ifndef FLUTTER_WEBRTC_RTC_GET_USERMEDIA_HXX
#define FLUTTER_WEBRTC_RTC_GET_USERMEDIA_HXX

#include "flutter_common.h"
#include "flutter_webrtc_base.h"

#ifdef _WIN32
// Forward-декларируем IMMDeviceEnumerator / IMMNotificationClient — чтобы не
// тянуть mmdeviceapi.h в публичный заголовок плагина (он используется во всех
// .cc, включая не-Win-сборки).
struct IMMDeviceEnumerator;
#endif

namespace flutter_webrtc_plugin {

#ifdef _WIN32
class MMDeviceNotificationClient;  // .cc-only impl, см. flutter_media_stream.cc
#endif

class FlutterMediaStream {
 public:
  FlutterMediaStream(FlutterWebRTCBase* base);
  ~FlutterMediaStream();

  void GetUserMedia(const EncodableMap& constraints,
                    std::unique_ptr<MethodResultProxy> result);

  void GetUserAudio(const EncodableMap& constraints,
                    scoped_refptr<RTCMediaStream> stream,
                    EncodableMap& params);

  void GetUserVideo(const EncodableMap& constraints,
                    scoped_refptr<RTCMediaStream> stream,
                    EncodableMap& params);

  void GetSources(std::unique_ptr<MethodResultProxy> result);

  void SelectAudioOutput(const std::string& device_id,
                         std::unique_ptr<MethodResultProxy> result);

  // 3-арг форма: label-fallback match + forceTrySet флаг (для будущего
  // hot-swap во время playout/recording). Совместима с 1-арг —
  // диспатчер вызывает её с label="", force_try_set=false.
  void SelectAudioOutput(const std::string& device_id,
                         const std::string& label,
                         bool force_try_set,
                         std::unique_ptr<MethodResultProxy> result);

  void SelectAudioInput(const std::string& device_id,
                        std::unique_ptr<MethodResultProxy> result);

  void SelectAudioInput(const std::string& device_id,
                        const std::string& label,
                        bool force_try_set,
                        std::unique_ptr<MethodResultProxy> result);

  void MediaStreamGetTracks(const std::string& stream_id,
                            std::unique_ptr<MethodResultProxy> result);

  void MediaStreamDispose(const std::string& stream_id,
                          std::unique_ptr<MethodResultProxy> result);

  void MediaStreamTrackSetEnable(const std::string& track_id,
                                 std::unique_ptr<MethodResultProxy> result);

  void MediaStreamTrackSwitchCamera(const std::string& track_id,
                                    std::unique_ptr<MethodResultProxy> result);

  void MediaStreamTrackDispose(const std::string& track_id,
                               std::unique_ptr<MethodResultProxy> result);

  void CreateLocalMediaStream(std::unique_ptr<MethodResultProxy> result);

  void OnDeviceChange();

 private:
  FlutterWebRTCBase* base_;

#ifdef _WIN32
  // Свой IMMNotificationClient — libwebrtc'шный audio_device_->OnDeviceChange
  // не дёргается на Win при physical unplug (USB/BT). Регистрируем COM
  // коллбэк прямо в плагине, эмитим event "onDeviceChange" в Dart — далее
  // вся цепочка `navigator.mediaDevices.ondevicechange` в SipService уже
  // готова. Оба указателя owned по AddRef/Release.
  IMMDeviceEnumerator* mm_enumerator_ = nullptr;
  MMDeviceNotificationClient* mm_notification_client_ = nullptr;
#endif
};

}  // namespace flutter_webrtc_plugin

#endif  // !FLUTTER_WEBRTC_RTC_GET_USERMEDIA_HXX
