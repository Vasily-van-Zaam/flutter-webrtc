#include "rtc_audio_device_impl.h"

#include <iostream>

namespace libwebrtc {

AudioDeviceImpl::AudioDeviceImpl(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module,
    webrtc::Thread* worker_thread)
    : audio_device_module_(audio_device_module), worker_thread_(worker_thread) {}

AudioDeviceImpl::~AudioDeviceImpl() {
  std::cout << "[AudioDeviceImpl] dtor" << std::endl;
}

int16_t AudioDeviceImpl::PlayoutDevices() {
  return audio_device_module_->PlayoutDevices();
}

int16_t AudioDeviceImpl::RecordingDevices() {
  return audio_device_module_->RecordingDevices();
}

int32_t AudioDeviceImpl::PlayoutDeviceName(uint16_t index,
                                           char name[kAdmMaxDeviceNameSize],
                                           char guid[kAdmMaxGuidSize]) {
  return worker_thread_->BlockingCall([&] {
    return audio_device_module_->PlayoutDeviceName(index, name, guid);
  });
}

int32_t AudioDeviceImpl::RecordingDeviceName(uint16_t index,
                                             char name[kAdmMaxDeviceNameSize],
                                             char guid[kAdmMaxGuidSize]) {
  return worker_thread_->BlockingCall([&] {
    return audio_device_module_->RecordingDeviceName(index, name, guid);
  });
}

int32_t AudioDeviceImpl::SetPlayoutDevice(uint16_t index) {
  std::cout << "[AudioDeviceImpl] SetPlayoutDevice index=" << index << std::endl;
  worker_thread_->PostTask([this, index] {
    if (audio_device_module_->Playing()) {
      audio_device_module_->StopPlayout();
      audio_device_module_->SetPlayoutDevice(index);
      audio_device_module_->InitPlayout();
      audio_device_module_->StartPlayout();
    } else {
      audio_device_module_->SetPlayoutDevice(index);
    }
  });
  return 0;
}

int32_t AudioDeviceImpl::SetRecordingDevice(uint16_t index) {
  std::cout << "[AudioDeviceImpl] SetRecordingDevice index=" << index << std::endl;
  worker_thread_->PostTask([this, index] {
    if (audio_device_module_->Recording()) {
      audio_device_module_->StopRecording();
      audio_device_module_->SetRecordingDevice(index);
      audio_device_module_->InitRecording();
      audio_device_module_->StartRecording();
    } else {
      audio_device_module_->SetRecordingDevice(index);
    }
  });
  return 0;
}

int32_t AudioDeviceImpl::SetMicrophoneVolume(uint32_t volume) {
  return worker_thread_->BlockingCall([&, volume] {
    return audio_device_module_->SetMicrophoneVolume(volume);
  });
}

int32_t AudioDeviceImpl::MicrophoneVolume(uint32_t& volume) {
  uint32_t* volume_ = &volume;
  return worker_thread_->BlockingCall([&, volume_] {
    return audio_device_module_->MicrophoneVolume(volume_);
  });
}

int32_t AudioDeviceImpl::SetSpeakerVolume(uint32_t volume) {
  return worker_thread_->BlockingCall([&, volume] {
    return audio_device_module_->SetSpeakerVolume(volume);
  });
}

int32_t AudioDeviceImpl::SpeakerVolume(uint32_t& volume) {
  uint32_t* volume_ = &volume;
  return worker_thread_->BlockingCall([&, volume_] {
    return audio_device_module_->SpeakerVolume(volume_);
  });
}

int32_t AudioDeviceImpl::OnDeviceChange(OnDeviceChangeCallback listener) {
  listener_ = listener;
  return 0;
}

// --- Сохранение и переприменение выбранного устройства ---

bool AudioDeviceImpl::SaveAndApplyPlayoutDevice(const std::string& device_id,
                                                bool force_try_set) {
  if (device_id.empty() || device_id == "default") {
    std::cout << "[AudioDeviceImpl] SaveAndApplyPlayoutDevice: empty/default → skip"
              << std::endl;
    return false;
  }

  saved_playout_device_id_ = device_id;

  int16_t n_devices = PlayoutDevices();
  char deviceName[256];
  char deviceGuid[256];

  for (int i = 0; i < n_devices; i++) {
    PlayoutDeviceName(i, deviceName, deviceGuid);
    // Сравниваем device_id с guid — тот же формат что в SelectAudioOutput
    // (SanitizeDeviceIdFromAudioBuffers(name, guid) → guid).
    std::string cur_guid = (deviceGuid != nullptr && strlen(deviceGuid) > 0)
                               ? std::string(deviceGuid)
                               : std::string(deviceName != nullptr ? deviceName : "");
    if (device_id == cur_guid) {
      std::cout << "[AudioDeviceImpl] SaveAndApplyPlayoutDevice: matched i=" << i
                << " id=" << device_id << " force=" << force_try_set << std::endl;

      if (force_try_set && n_devices > 1) {
        // Double-switch для пробуждения MF ADM после hold→unhold
        SetPlayoutDevice(0);
      }
      SetPlayoutDevice(i);
      return true;
    }
  }

  std::cout << "[AudioDeviceImpl] SaveAndApplyPlayoutDevice: deviceId="
            << device_id << " NOT found in " << n_devices << " ADM devices"
            << std::endl;
  return false;
}

bool AudioDeviceImpl::SaveAndApplyRecordingDevice(const std::string& device_id) {
  if (device_id.empty() || device_id == "default") {
    std::cout << "[AudioDeviceImpl] SaveAndApplyRecordingDevice: empty/default → skip"
              << std::endl;
    return false;
  }

  saved_recording_device_id_ = device_id;

  int16_t n_devices = RecordingDevices();
  char deviceName[256];
  char deviceGuid[256];

  for (int i = 0; i < n_devices; i++) {
    RecordingDeviceName(i, deviceName, deviceGuid);
    // Сравниваем device_id с guid — тот же формат что в SelectAudioInput.
    std::string cur_guid = (deviceGuid != nullptr && strlen(deviceGuid) > 0)
                               ? std::string(deviceGuid)
                               : std::string(deviceName != nullptr ? deviceName : "");
    if (device_id == cur_guid) {
      std::cout << "[AudioDeviceImpl] SaveAndApplyRecordingDevice: matched i=" << i
                << " id=" << device_id << std::endl;
      SetRecordingDevice(i);
      return true;
    }
  }

  std::cout << "[AudioDeviceImpl] SaveAndApplyRecordingDevice: deviceId="
            << device_id << " NOT found in " << n_devices << " ADM devices"
            << std::endl;
  return false;
}

}  // namespace libwebrtc
