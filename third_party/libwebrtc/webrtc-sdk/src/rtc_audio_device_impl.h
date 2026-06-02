#ifndef LIB_WEBRTC_AUDIO_DEVICE_IMPL_HXX
#define LIB_WEBRTC_AUDIO_DEVICE_IMPL_HXX

#include <string>

#include "modules/audio_device/audio_device_impl.h"
#include "modules/audio_device/include/audio_device.h"
#include "rtc_audio_device.h"
#include "rtc_base/ref_count.h"
#include "rtc_base/thread.h"

namespace libwebrtc {

class AudioDeviceImpl : public RTCAudioDevice {
 public:
  AudioDeviceImpl(
      webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module,
      webrtc::Thread* worker_thread);

  virtual ~AudioDeviceImpl();

 public:
  int16_t PlayoutDevices() override;

  int16_t RecordingDevices() override;

  int32_t PlayoutDeviceName(uint16_t index, char name[kAdmMaxDeviceNameSize],
                            char guid[kAdmMaxGuidSize]) override;

  int32_t RecordingDeviceName(uint16_t index, char name[kAdmMaxDeviceNameSize],
                              char guid[kAdmMaxGuidSize]) override;

  int32_t SetPlayoutDevice(uint16_t index) override;

  int32_t SetRecordingDevice(uint16_t index) override;

  int32_t SetMicrophoneVolume(uint32_t volume) override;

  int32_t MicrophoneVolume(uint32_t& volume) override;

  int32_t SetSpeakerVolume(uint32_t volume) override;

  int32_t SpeakerVolume(uint32_t& volume) override;

  int32_t OnDeviceChange(OnDeviceChangeCallback listener) override;

  // Сохраняет выбранный deviceId и переприменяет его:
  // ищет индекс по deviceId в текущем списке ADM, вызывает SetPlayoutDevice.
  // Вызывать при unhold, ondevicechange, StartPlayout.
  // Возвращает true если устройство найдено и применено.
  bool SaveAndApplyPlayoutDevice(const std::string& device_id,
                                 bool force_try_set = false);

  // То же для recording (микрофон).
  bool SaveAndApplyRecordingDevice(const std::string& device_id);

  // Публичный доступ к worker_thread_ для FlutterMediaStream.
  webrtc::Thread* worker_thread() { return worker_thread_; }

  // Публичный доступ к нативному ADM.
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm() {
    return audio_device_module_;
  }

  // Простое сохранение device_id без поиска индекса — для вызова
  // после успешного SetPlayoutDevice/SetRecordingDevice.
  void set_saved_playout_device_id(const std::string& id) {
    saved_playout_device_id_ = id;
  }
  void set_saved_recording_device_id(const std::string& id) {
    saved_recording_device_id_ = id;
  }

  // Доступ к сохранённым deviceId.
  const std::string& saved_playout_device_id() const {
    return saved_playout_device_id_;
  }
  const std::string& saved_recording_device_id() const {
    return saved_recording_device_id_;
  }

 private:
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module_;
  webrtc::Thread* worker_thread_ = nullptr;
  OnDeviceChangeCallback listener_ = nullptr;

  // Сохранённые deviceId выбранные пользователем.
  std::string saved_playout_device_id_;
  std::string saved_recording_device_id_;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_AUDIO_DEVICE_IMPL_HXX
