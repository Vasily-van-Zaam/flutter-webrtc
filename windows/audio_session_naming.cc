// Windows-only: даём WASAPI-сессиям нашего процесса дружелюбное имя
// в Volume Mixer (sndvol).
//
// Почему оно надо: libwebrtc Win ADM открывает IAudioClient::Initialize
// **без** последующего IAudioSessionControl::SetDisplayName, поэтому
// Mixer показывает имя exe («synergy_call_center.exe») как заголовок
// сессии. Это и косметика, и privacy: оператору лучше видеть «Synergy
// Call Center» и понимать какой именно слайдер он крутит. Аналогично
// делают Discord/Slack/Teams со своими капчер/плейаут сессиями.
//
// Стратегия:
//   1. Перечисляем все активные IMMDevice (рендер + захват)
//   2. Для каждого получаем IAudioSessionManager2 → IAudioSessionEnumerator
//   3. Для каждой сессии получаем IAudioSessionControl2, фильтруем
//      по `GetProcessId() == GetCurrentProcessId()` и
//      `IsSystemSoundsSession()==FALSE`
//   4. Кастим обратно в IAudioSessionControl и зовём SetDisplayName
//
// Поток: вызывать с UI или main thread'а (COM в STA). На worker'ах
// можно тоже, но придётся CoInitializeEx — мы не лезем чтобы не
// конфликтовать с тем что уже инициализировано Flutter'ом.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <combaseapi.h>
#include <wrl/client.h>

#include <string>

#include "audio_session_naming.h"

namespace flutter_webrtc_plugin {

namespace {

using Microsoft::WRL::ComPtr;

// Применяет имя ко всем сессиям нашего PID на одном IMMDevice.
// Возвращает число успешно переименованных сессий.
int ApplyDisplayNameForDevice(IMMDevice* device, const std::wstring& name) {
  if (!device) return 0;

  ComPtr<IAudioSessionManager2> sessionManager;
  HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2),
                                CLSCTX_ALL,
                                nullptr,
                                reinterpret_cast<void**>(sessionManager.GetAddressOf()));
  if (FAILED(hr)) return 0;

  ComPtr<IAudioSessionEnumerator> enumerator;
  hr = sessionManager->GetSessionEnumerator(enumerator.GetAddressOf());
  if (FAILED(hr)) return 0;

  int count = 0;
  if (FAILED(enumerator->GetCount(&count))) return 0;

  const DWORD ourPid = GetCurrentProcessId();
  int renamed = 0;

  for (int i = 0; i < count; ++i) {
    ComPtr<IAudioSessionControl> sessionControl;
    if (FAILED(enumerator->GetSession(i, sessionControl.GetAddressOf()))) {
      continue;
    }

    ComPtr<IAudioSessionControl2> sessionControl2;
    if (FAILED(sessionControl.As(&sessionControl2))) {
      continue;
    }

    // Системные звуки — не трогаем, иначе перепишем общий слайдер
    // «Системные звуки» в Mixer на «Synergy Call Center».
    if (sessionControl2->IsSystemSoundsSession() == S_OK) {
      continue;
    }

    DWORD pid = 0;
    if (FAILED(sessionControl2->GetProcessId(&pid))) continue;
    if (pid != ourPid) continue;

    // SetDisplayName идёт через базовый IAudioSessionControl.
    // Второй параметр (EventContext) — опциональный GUID для фильтрации
    // последующих OnDisplayNameChanged — не нужен, NULL.
    hr = sessionControl->SetDisplayName(name.c_str(), nullptr);
    if (SUCCEEDED(hr)) {
      ++renamed;
    }
  }

  return renamed;
}

}  // namespace

int SetAudioSessionDisplayName(const std::wstring& display_name) {
  if (display_name.empty()) return 0;

  ComPtr<IMMDeviceEnumerator> deviceEnumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                nullptr,
                                CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(deviceEnumerator.GetAddressOf()));
  if (FAILED(hr)) return 0;

  int total = 0;

  // Render + Capture — обе стороны нужны: голос пишется через capture
  // (микрофон), играется через render (выход). Оба IAudioClient'а
  // создают свою WASAPI-сессию в Mixer'е.
  const EDataFlow flows[] = {eRender, eCapture};
  for (EDataFlow flow : flows) {
    ComPtr<IMMDeviceCollection> collection;
    hr = deviceEnumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              collection.GetAddressOf());
    if (FAILED(hr)) continue;

    UINT deviceCount = 0;
    if (FAILED(collection->GetCount(&deviceCount))) continue;

    for (UINT i = 0; i < deviceCount; ++i) {
      ComPtr<IMMDevice> device;
      if (FAILED(collection->Item(i, device.GetAddressOf()))) continue;
      total += ApplyDisplayNameForDevice(device.Get(), display_name);
    }
  }

  return total;
}

}  // namespace flutter_webrtc_plugin
