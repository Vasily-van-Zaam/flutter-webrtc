#include "flutter_media_stream.h"

#include <iostream>

#include "flutter_utf8_sanitize.h"

#ifdef _WIN32
// Windows Core Audio API — гарантированно видит ВСЕ системные
// audio-устройства, ту же информацию что показывает Volume Mixer и
// панель «Звук» Windows. Используется как fallback когда WebRTC ADM
// (`audio_device_->RecordingDevices()`) возвращает 0 — это случается
// на ряде Windows-конфигураций (виртуальные/redirected устройства,
// проблемы с инициализацией ADM до первого peer connection).
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <combaseapi.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <vector>
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#endif

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FPS 30

namespace flutter_webrtc_plugin {

namespace {

std::string SanitizeDeviceIdFromAudioBuffers(const char* name, const char* guid) {
  const std::string raw = (guid != nullptr && strlen(guid) > 0)
                              ? std::string(guid)
                              : std::string(name != nullptr ? name : "");
  return SanitizeUtf8ForFlutter(raw);
}

std::string SanitizeLabel(const char* name) {
  return SanitizeUtf8ForFlutter(std::string(name != nullptr ? name : ""));
}

std::string SanitizeDeviceIdFromVideoBuffers(const char* name, const char* guid) {
  const std::string raw = (guid != nullptr && strlen(guid) > 0)
                              ? std::string(guid)
                              : std::string(name != nullptr ? name : "");
  return SanitizeUtf8ForFlutter(raw);
}

#ifdef _WIN32

// UTF-16 → UTF-8 для Windows Core Audio API (WCHAR* строки).
std::string WideToUtf8(const wchar_t* wstr) {
  if (!wstr) return "";
  int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr,
                                 nullptr);
  if (len <= 1) return "";
  std::string result(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr,
                      nullptr);
  return result;
}

// COM init — безопасно вызывать многократно. Flutter Windows host обычно
// уже инициализирует COM, тогда вернётся S_FALSE; мы не делаем
// CoUninitialize чтобы не сломать чужие COM-объекты на том же thread'е.
void EnsureComInitialized() {
  static thread_local bool tried = false;
  if (tried) return;
  tried = true;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

// Получить deviceId Windows-default endpoint'а для указанной flow+role.
// `eMultimedia` для рендера = то что показано в Sound Mixer'е как
// активное выходное устройство. `eCommunications` для capture = то
// что Mixer показывает как input default (для BT-наушников это HFP-
// эндпойнт «Головной телефон (BT)» с микрофоном; eMultimedia для
// capture часто тот же самый, но не всегда). Пустая строка если default
// не задан или COM-вызов не удался.
std::string GetWindowsDefaultEndpointId(EDataFlow flow, ERole role) {
  EnsureComInitialized();
  IMMDeviceEnumerator* pEnumerator = nullptr;
  HRESULT hr =
      CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                       __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
  if (FAILED(hr) || !pEnumerator) return "";

  IMMDevice* pDefault = nullptr;
  hr = pEnumerator->GetDefaultAudioEndpoint(flow, role, &pDefault);
  pEnumerator->Release();
  if (FAILED(hr) || !pDefault) return "";

  LPWSTR idW = nullptr;
  std::string result;
  if (SUCCEEDED(pDefault->GetId(&idW)) && idW) {
    result = WideToUtf8(idW);
    CoTaskMemFree(idW);
  }
  pDefault->Release();
  return result;
}

// Перечислить Windows audio endpoints (microphones или speakers) через
// IMMDeviceEnumerator и добавить в `sources`. Все добавленные deviceId
// также сохраняем в `seen_ids` для последующего дедупа с ADM-based
// перечислением. Возвращает количество добавленных устройств.
//
// **Default-endpoint ставится первым в списке** (если найден среди
// active-devices). Это нужно чтобы Dart-side `_enumerateDevicesByKind`
// логика «первое устройство = default» совпадала с тем что показывает
// Windows Volume Mixer / Sound settings.
//   * Для render (output) используется `eMultimedia` — что показано в
//     Mixer как «Устройство вывода».
//   * Для capture (input) используется `eCommunications` — что показано
//     в Mixer как «Устройство ввода» для voice-режима (для BT — HFP с
//     микрофоном, а не A2DP «Наушники» без mic'а).
int EnumerateWindowsAudioEndpoints(EncodableList& sources,
                                    std::set<std::string>& seen_ids,
                                    EDataFlow flow,
                                    const char* kind) {
  EnsureComInitialized();

  IMMDeviceEnumerator* pEnumerator = nullptr;
  HRESULT hr =
      CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                       __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
  if (FAILED(hr) || !pEnumerator) return 0;

  IMMDeviceCollection* pCollection = nullptr;
  // ACTIVE | UNPLUGGED — нужно для BT-наушников. Bluetooth-устройство
  // имеет два endpoint'а (A2DP «Наушники» и HFP «Головной телефон»).
  // Когда BT-стек в A2DP-режиме (стерео-музыка), HFP-endpoint
  // переходит в `DEVICE_STATE_UNPLUGGED` (как jack без кабеля).
  // Windows Volume Mixer всё равно показывает его в списке — он
  // транспарентно перейдёт в ACTIVE как только приложение откроет
  // capture (BT-stack switch'нёт профиль). Если фильтровать только
  // ACTIVE — оператор видит «исчезнувший» AirPods Pro микрофон в
  // нашем dropdown'е после первого звонка/idle (HFP отвалился, A2DP
  // активен). DISABLED (user отключил) и NOTPRESENT (железо
  // отсутствует) НЕ включаем — это реальная недоступность.
  hr = pEnumerator->EnumAudioEndpoints(
      flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &pCollection);
  if (FAILED(hr) || !pCollection) {
    pEnumerator->Release();
    return 0;
  }

  // Получаем Windows-default endpoint ДО enumerate чтобы знать какой
  // device поставить первым.
  const ERole defaultRole = (flow == eRender) ? eMultimedia : eCommunications;
  const std::string defaultId = GetWindowsDefaultEndpointId(flow, defaultRole);
  const std::string sanitizedDefaultId =
      defaultId.empty() ? "" : SanitizeUtf8ForFlutter(defaultId);

  UINT count = 0;
  pCollection->GetCount(&count);

  // Сначала собираем все устройства в локальный вектор, потом
  // переупорядочиваем (default → первым). Это позволяет emit'ить
  // в `sources` уже в корректном порядке без вставок в середину.
  struct Endpoint {
    std::string sanitizedId;
    std::string label;
  };
  std::vector<Endpoint> endpoints;
  endpoints.reserve(count);

  for (UINT i = 0; i < count; i++) {
    IMMDevice* pDevice = nullptr;
    if (FAILED(pCollection->Item(i, &pDevice)) || !pDevice) continue;

    LPWSTR deviceIdW = nullptr;
    std::string deviceId;
    if (SUCCEEDED(pDevice->GetId(&deviceIdW)) && deviceIdW) {
      deviceId = WideToUtf8(deviceIdW);
      CoTaskMemFree(deviceIdW);
    }

    // FriendlyName — это то что показывается в Sound панели и Volume
    // Mixer (например «Микрофон (Realtek High Definition Audio)»).
    std::string label;
    IPropertyStore* pProps = nullptr;
    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
      PROPVARIANT varName;
      PropVariantInit(&varName);
      if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) &&
          varName.vt == VT_LPWSTR && varName.pwszVal) {
        label = WideToUtf8(varName.pwszVal);
      }
      PropVariantClear(&varName);
      pProps->Release();
    }

    pDevice->Release();

    if (deviceId.empty()) continue;

    endpoints.push_back({
        SanitizeUtf8ForFlutter(deviceId),
        SanitizeUtf8ForFlutter(label),
    });
  }
  pCollection->Release();
  pEnumerator->Release();

  // Stable-partition: если default найден среди endpoints — перенести
  // его в начало (порядок остальных сохраняется).
  if (!sanitizedDefaultId.empty()) {
    auto it = std::find_if(endpoints.begin(), endpoints.end(),
                            [&](const Endpoint& e) {
                              return e.sanitizedId == sanitizedDefaultId;
                            });
    if (it != endpoints.end() && it != endpoints.begin()) {
      Endpoint def = std::move(*it);
      endpoints.erase(it);
      endpoints.insert(endpoints.begin(), std::move(def));
    }
  }

  int added = 0;
  for (const auto& e : endpoints) {
    EncodableMap audio;
    audio[EncodableValue("label")] = EncodableValue(e.label);
    audio[EncodableValue("deviceId")] = EncodableValue(e.sanitizedId);
    audio[EncodableValue("facing")] = "";
    audio[EncodableValue("kind")] = kind;
    sources.push_back(EncodableValue(audio));
    seen_ids.insert(e.sanitizedId);
    added++;
  }
  return added;
}

#endif  // _WIN32

}  // namespace

#ifdef _WIN32
// Собственный IMMNotificationClient — нужен потому что libwebrtc'шный
// `audio_device_->OnDeviceChange` на Win НЕ срабатывает на physical unplug
// (USB/BT). Подтверждено логом 2026-05-18: ни одного `[Audio] ondevicechange`
// после отсоединения наушников. Это лечится не в libwebrtc.dll (который
// пересобирать долго и больно), а на нашем уровне — `IMMNotificationClient`
// это чисто COM-интерфейс системы, регистрируется через
// `IMMDeviceEnumerator::RegisterEndpointNotificationCallback`. Плагину
// достаточно реализовать его и эмитить в event_channel один и тот же payload
// `{"event":"onDeviceChange"}` — Dart-сторона уже всё умеет обрабатывать
// через `navigator.mediaDevices.ondevicechange` (см. SipService).
//
// Thread-safety: коллбэки IMMNotificationClient приходят на random COM
// worker thread. Звать `EventChannelProxy::Success` отсюда безопасно —
// внутри него уже `task_runner_->EnqueueTask(...)` маршалит доставку
// sink->Success на Flutter UI thread (см. flutter_common.cc:136-148).
class MMDeviceNotificationClient : public IMMNotificationClient {
 public:
  explicit MMDeviceNotificationClient(FlutterWebRTCBase* base)
      : base_(base),
        ref_(1),
        alive_(std::make_shared<std::atomic<bool>>(true)) {}

  // Внешний accessor для FlutterMediaStream::dtor — снимаем флаг ДО
  // UnregisterEndpointNotificationCallback. Если коллбэк уже летит в COM
  // worker thread'е, он увидит alive=false и пропустит обращение к base_.
  std::shared_ptr<std::atomic<bool>> alive_handle() const { return alive_; }

  // IUnknown
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&ref_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
      delete this;
    }
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IMMNotificationClient)) {
      *ppv = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  // IMMNotificationClient — все четыре релевантных коллбэка эмитят один и
  // тот же event. Dart дебаунсит серию (500ms) и делает один refresh.
  HRESULT STDMETHODCALLTYPE
  OnDeviceStateChanged(LPCWSTR /*deviceId*/, DWORD newState) override {
    std::cout << "[FlutterWebRTC] MMNotification: DeviceStateChanged state=0x"
              << std::hex << newState << std::dec << std::endl;
    Emit();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*deviceId*/) override {
    std::cout << "[FlutterWebRTC] MMNotification: DeviceAdded" << std::endl;
    Emit();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*deviceId*/) override {
    std::cout << "[FlutterWebRTC] MMNotification: DeviceRemoved" << std::endl;
    Emit();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,
                                                   ERole role,
                                                   LPCWSTR /*deviceId*/) override {
    std::cout << "[FlutterWebRTC] MMNotification: DefaultDeviceChanged flow="
              << flow << " role=" << role << std::endl;
    Emit();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  OnPropertyValueChanged(LPCWSTR /*deviceId*/, const PROPERTYKEY /*key*/) override {
    // Спамит на каждый property update (volume, format change, etc) — нам
    // это не нужно, audio-device-state мы получаем через State/Added/Removed.
    return S_OK;
  }

 private:
  void Emit() {
    // Guard для случая, когда FlutterMediaStream::~ уже отметил нас как
    // мёртвых, но Windows core audio thread всё ещё в полёте с callback'ом.
    if (!alive_->load()) return;
    EncodableMap info;
    info[EncodableValue("event")] = "onDeviceChange";
    base_->event_channel()->Success(EncodableValue(info), false);
  }

  FlutterWebRTCBase* base_;
  LONG ref_;
  std::shared_ptr<std::atomic<bool>> alive_;
};
#endif  // _WIN32

FlutterMediaStream::FlutterMediaStream(FlutterWebRTCBase* base) : base_(base) {
  // Capture `base` by value (pointer copy) instead of `[&]` (raw `this`).
  // The original `[&]` capture causes use-after-free on Windows when
  // OnDeviceChange fires after FlutterMediaStream is destroyed — the lambda
  // would dereference the dead `this` to reach `base_`.
  // See: https://github.com/flutter/flutter/issues/118611
  base_->audio_device_->OnDeviceChange([base] {
    EncodableMap info;
    info[EncodableValue("event")] = "onDeviceChange";
    base->event_channel()->Success(EncodableValue(info), false);
  });

#ifdef _WIN32
  // Регистрируем свой IMMNotificationClient. См. комментарий над классом
  // MMDeviceNotificationClient — это покрывает гэп с libwebrtc, который
  // на физический unplug на Win не реагирует.
  EnsureComInitialized();
  HRESULT hr = CoCreateInstance(
      __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator),
      reinterpret_cast<void**>(&mm_enumerator_));
  if (SUCCEEDED(hr) && mm_enumerator_) {
    mm_notification_client_ = new MMDeviceNotificationClient(base_);
    hr = mm_enumerator_->RegisterEndpointNotificationCallback(
        mm_notification_client_);
    if (FAILED(hr)) {
      std::cout << "[FlutterWebRTC] RegisterEndpointNotificationCallback "
                   "failed hr=0x"
                << std::hex << hr << std::dec << std::endl;
      mm_notification_client_->Release();
      mm_notification_client_ = nullptr;
      mm_enumerator_->Release();
      mm_enumerator_ = nullptr;
    } else {
      std::cout << "[FlutterWebRTC] MMNotificationClient registered"
                << std::endl;
    }
  } else {
    std::cout << "[FlutterWebRTC] CoCreateInstance(MMDeviceEnumerator) "
                 "failed hr=0x"
              << std::hex << hr << std::dec << std::endl;
  }
#endif
}

FlutterMediaStream::~FlutterMediaStream() {
#ifdef _WIN32
  // 1. Помечаем notification client мёртвым ДО Unregister — это закрывает
  //    окно гонки с in-flight callback'ом на COM worker thread'е (он
  //    проверяет alive_ перед обращением к base_->event_channel()).
  if (mm_notification_client_) {
    auto alive = mm_notification_client_->alive_handle();
    if (alive) alive->store(false);
  }
  // 2. Снимаем регистрацию. Windows core audio service после этого вызова
  //    больше не дёргает наш callback. Не строго блокирующий — coll'ы,
  //    которые уже летят на других thread'ах, увидят alive=false в (1)
  //    и просто ретурнят без обращения к dying state.
  if (mm_enumerator_ && mm_notification_client_) {
    mm_enumerator_->UnregisterEndpointNotificationCallback(
        mm_notification_client_);
  }
  // 3. Release: Windows core audio мог держать свой AddRef — наш Release
  //    может НЕ быть последним (delete this отложится), это ок.
  if (mm_notification_client_) {
    mm_notification_client_->Release();
    mm_notification_client_ = nullptr;
  }
  if (mm_enumerator_) {
    mm_enumerator_->Release();
    mm_enumerator_ = nullptr;
  }
#endif
}

void FlutterMediaStream::GetUserMedia(
    const EncodableMap& constraints,
    std::unique_ptr<MethodResultProxy> result) {
  std::string uuid = base_->GenerateUUID();
  scoped_refptr<RTCMediaStream> stream =
      base_->factory_->CreateStream(uuid.c_str());

  EncodableMap params;
  params[EncodableValue("streamId")] = EncodableValue(uuid);

  auto it = constraints.find(EncodableValue("audio"));
  if (it != constraints.end()) {
    EncodableValue audio = it->second;
    if (TypeIs<bool>(audio)) {
      if (true == GetValue<bool>(audio)) {
        GetUserAudio(constraints, stream, params);
      }
    } else if (TypeIs<EncodableMap>(audio)) {
      GetUserAudio(constraints, stream, params);
    } else {
      params[EncodableValue("audioTracks")] = EncodableValue(EncodableList());
    }
  } else {
    params[EncodableValue("audioTracks")] = EncodableValue(EncodableList());
  }

  it = constraints.find(EncodableValue("video"));
  params[EncodableValue("videoTracks")] = EncodableValue(EncodableList());
  if (it != constraints.end()) {
    EncodableValue video = it->second;
    if (TypeIs<bool>(video)) {
      if (true == GetValue<bool>(video)) {
        GetUserVideo(constraints, stream, params);
      }
    } else if (TypeIs<EncodableMap>(video)) {
      GetUserVideo(constraints, stream, params);
    }
  }

  base_->local_streams_[uuid] = stream;
  result->Success(EncodableValue(params));
}

void addDefaultAudioConstraints(
    scoped_refptr<RTCMediaConstraints> audioConstraints) {
  audioConstraints->AddOptionalConstraint("googNoiseSuppression", "true");
  audioConstraints->AddOptionalConstraint("googEchoCancellation", "true");
  audioConstraints->AddOptionalConstraint("echoCancellation", "true");
  audioConstraints->AddOptionalConstraint("googEchoCancellation2", "true");
  audioConstraints->AddOptionalConstraint("googDAEchoCancellation", "true");
}

std::string getSourceIdConstraint(const EncodableMap& mediaConstraints) {
  auto it = mediaConstraints.find(EncodableValue("optional"));
  if (it != mediaConstraints.end() && TypeIs<EncodableList>(it->second)) {
    EncodableList optional = GetValue<EncodableList>(it->second);
    for (size_t i = 0, size = optional.size(); i < size; i++) {
      if (TypeIs<EncodableMap>(optional[i])) {
        EncodableMap option = GetValue<EncodableMap>(optional[i]);
        auto it2 = option.find(EncodableValue("sourceId"));
        if (it2 != option.end() && TypeIs<std::string>(it2->second)) {
          return GetValue<std::string>(it2->second);
        }
      }
    }
  }
  return "";
}

std::string getDeviceIdConstraint(const EncodableMap& mediaConstraints) {
  auto it = mediaConstraints.find(EncodableValue("deviceId"));
  if (it != mediaConstraints.end() && TypeIs<std::string>(it->second)) {
    return GetValue<std::string>(it->second);
  }
  return "";
}

void FlutterMediaStream::GetUserAudio(const EncodableMap& constraints,
                                      scoped_refptr<RTCMediaStream> stream,
                                      EncodableMap& params) {
  bool enable_audio = false;
  scoped_refptr<RTCMediaConstraints> audioConstraints;
  std::string sourceId;
  std::string deviceId;
  auto it = constraints.find(EncodableValue("audio"));
  if (it != constraints.end()) {
    EncodableValue audio = it->second;
    if (TypeIs<bool>(audio)) {
      audioConstraints = RTCMediaConstraints::Create();
      addDefaultAudioConstraints(audioConstraints);
      enable_audio = GetValue<bool>(audio);
      sourceId = "";
      deviceId = "";
    }
    if (TypeIs<EncodableMap>(audio)) {
      EncodableMap localMap = GetValue<EncodableMap>(audio);
      sourceId = getSourceIdConstraint(localMap);
      deviceId = getDeviceIdConstraint(localMap);
      audioConstraints = base_->ParseMediaConstraints(localMap);
      enable_audio = true;
    }
  }

  // Selecting audio input device by sourceId and audio output device by
  // deviceId

  if (enable_audio) {
    char strRecordingName[256];
    char strRecordingGuid[256];
    int playout_devices = base_->audio_device_->PlayoutDevices();
    int recording_devices = base_->audio_device_->RecordingDevices();

    for (uint16_t i = 0; i < recording_devices; i++) {
      base_->audio_device_->RecordingDeviceName(i, strRecordingName,
                                                strRecordingGuid);
      if (sourceId != "" &&
          sourceId ==
              SanitizeDeviceIdFromAudioBuffers(strRecordingName,
                                               strRecordingGuid)) {
        base_->audio_device_->SetRecordingDevice(i);
      }
    }

    if (sourceId == "" && recording_devices > 0) {
      // Guard: на ряде Windows-конфигураций ADM возвращает
      // RecordingDevices()=0 пока peer connection не активирован.
      // Без guard'а RecordingDeviceName(0, ...) уйдёт за границы.
      base_->audio_device_->RecordingDeviceName(0, strRecordingName,
                                                strRecordingGuid);
      sourceId = SanitizeDeviceIdFromAudioBuffers(strRecordingName,
                                                  strRecordingGuid);
    }

    char strPlayoutName[256];
    char strPlayoutGuid[256];
    for (uint16_t i = 0; i < playout_devices; i++) {
      base_->audio_device_->PlayoutDeviceName(i, strPlayoutName,
                                              strPlayoutGuid);
      if (deviceId != "" &&
          deviceId ==
              SanitizeDeviceIdFromAudioBuffers(strPlayoutName,
                                               strPlayoutGuid)) {
        base_->audio_device_->SetPlayoutDevice(i);
      }
    }

    scoped_refptr<RTCAudioSource> source =
        base_->factory_->CreateAudioSource("audio_input");
    std::string uuid = base_->GenerateUUID();
    scoped_refptr<RTCAudioTrack> track =
        base_->factory_->CreateAudioTrack(source, uuid.c_str());

    std::string track_id = track->id().std_string();

    EncodableMap track_info;
    track_info[EncodableValue("id")] = EncodableValue(track->id().std_string());
    track_info[EncodableValue("label")] =
        EncodableValue(track->id().std_string());
    track_info[EncodableValue("kind")] =
        EncodableValue(track->kind().std_string());
    track_info[EncodableValue("enabled")] = EncodableValue(track->enabled());

    EncodableMap settings;
    settings[EncodableValue("deviceId")] =
        EncodableValue(SanitizeUtf8ForFlutter(sourceId));
    settings[EncodableValue("kind")] = EncodableValue("audioinput");
    settings[EncodableValue("autoGainControl")] = EncodableValue(true);
    settings[EncodableValue("echoCancellation")] = EncodableValue(true);
    settings[EncodableValue("noiseSuppression")] = EncodableValue(true);
    settings[EncodableValue("channelCount")] = EncodableValue(1);
    settings[EncodableValue("latency")] = EncodableValue(0);
    track_info[EncodableValue("settings")] = EncodableValue(settings);

    EncodableList audioTracks;
    audioTracks.push_back(EncodableValue(track_info));
    params[EncodableValue("audioTracks")] = EncodableValue(audioTracks);
    stream->AddTrack(track);

    base_->local_tracks_[track->id().std_string()] = track;
  }
}

std::string getFacingMode(const EncodableMap& mediaConstraints) {
  return mediaConstraints.find(EncodableValue("facingMode")) !=
                 mediaConstraints.end()
             ? GetValue<std::string>(
                   mediaConstraints.find(EncodableValue("facingMode"))->second)
             : "";
}

EncodableValue getConstrainInt(const EncodableMap& constraints,
                               const std::string& key) {
  EncodableValue value;
  auto it = constraints.find(EncodableValue(key));
  if (it != constraints.end()) {
    if (TypeIs<int>(it->second)) {
      return it->second;
    }

    if (TypeIs<EncodableMap>(it->second)) {
      EncodableMap innerMap = GetValue<EncodableMap>(it->second);
      auto it2 = innerMap.find(EncodableValue("ideal"));
      if (it2 != innerMap.end() && TypeIs<int>(it2->second)) {
        return it2->second;
      }
    }
  }

  return EncodableValue();
}

void FlutterMediaStream::GetUserVideo(const EncodableMap& constraints,
                                      scoped_refptr<RTCMediaStream> stream,
                                      EncodableMap& params) {
  EncodableMap video_constraints;
  EncodableMap video_mandatory;
  auto it = constraints.find(EncodableValue("video"));
  if (it != constraints.end() && TypeIs<EncodableMap>(it->second)) {
    video_constraints = GetValue<EncodableMap>(it->second);
    if (video_constraints.find(EncodableValue("mandatory")) !=
        video_constraints.end()) {
      video_mandatory = GetValue<EncodableMap>(
          video_constraints.find(EncodableValue("mandatory"))->second);
    }
  }

  std::string facing_mode = getFacingMode(video_constraints);
  // bool isFacing = facing_mode == "" || facing_mode != "environment";
  std::string sourceId = getSourceIdConstraint(video_constraints);

  EncodableValue widthValue = getConstrainInt(video_constraints, "width");

  if (widthValue == EncodableValue())
    widthValue = findEncodableValue(video_mandatory, "minWidth");

  if (widthValue == EncodableValue())
    widthValue = findEncodableValue(video_mandatory, "width");

  EncodableValue heightValue = getConstrainInt(video_constraints, "height");

  if (heightValue == EncodableValue())
    heightValue = findEncodableValue(video_mandatory, "minHeight");

  if (heightValue == EncodableValue())
    heightValue = findEncodableValue(video_mandatory, "height");

  EncodableValue fpsValue = getConstrainInt(video_constraints, "frameRate");

  if (fpsValue == EncodableValue())
    fpsValue = findEncodableValue(video_mandatory, "minFrameRate");

  if (fpsValue == EncodableValue())
    fpsValue = findEncodableValue(video_mandatory, "frameRate");

  scoped_refptr<RTCVideoCapturer> video_capturer;
  char strNameUTF8[256];
  char strGuidUTF8[256];
  int nb_video_devices = base_->video_device_->NumberOfDevices();

  int32_t width = toInt(widthValue, DEFAULT_WIDTH);
  int32_t height = toInt(heightValue, DEFAULT_HEIGHT);
  int32_t fps = toInt(fpsValue, DEFAULT_FPS);

  for (int i = 0; i < nb_video_devices; i++) {
    base_->video_device_->GetDeviceName(i, strNameUTF8, 256, strGuidUTF8, 256);
    if (sourceId != "" &&
        sourceId ==
            SanitizeDeviceIdFromVideoBuffers(strNameUTF8, strGuidUTF8)) {
      video_capturer =
          base_->video_device_->Create(strNameUTF8, i, width, height, fps);
      break;
    }
  }

  if (nb_video_devices == 0)
    return;

  if (!video_capturer.get()) {
    base_->video_device_->GetDeviceName(0, strNameUTF8, 128, strGuidUTF8, 128);
    sourceId = SanitizeDeviceIdFromVideoBuffers(strNameUTF8, strGuidUTF8);
    video_capturer =
        base_->video_device_->Create(strNameUTF8, 0, width, height, fps);
  }

  if (!video_capturer.get())
    return;

  video_capturer->StartCapture();

  const char* video_source_label = "video_input";
  scoped_refptr<RTCVideoSource> source = base_->factory_->CreateVideoSource(
      video_capturer, video_source_label,
      base_->ParseMediaConstraints(video_constraints));

  std::string uuid = base_->GenerateUUID();
  scoped_refptr<RTCVideoTrack> track =
      base_->factory_->CreateVideoTrack(source, uuid.c_str());

  EncodableList videoTracks;
  EncodableMap info;
  info[EncodableValue("id")] = EncodableValue(track->id().std_string());
  info[EncodableValue("label")] = EncodableValue(track->id().std_string());
  info[EncodableValue("kind")] = EncodableValue(track->kind().std_string());
  info[EncodableValue("enabled")] = EncodableValue(track->enabled());

  EncodableMap settings;
  settings[EncodableValue("deviceId")] =
      EncodableValue(SanitizeUtf8ForFlutter(sourceId));
  settings[EncodableValue("kind")] = EncodableValue("videoinput");
  settings[EncodableValue("width")] = EncodableValue(width);
  settings[EncodableValue("height")] = EncodableValue(height);
  settings[EncodableValue("frameRate")] = EncodableValue(fps);
  info[EncodableValue("settings")] = EncodableValue(settings);

  videoTracks.push_back(EncodableValue(info));
  params[EncodableValue("videoTracks")] = EncodableValue(videoTracks);

  stream->AddTrack(track);

  base_->local_tracks_[track->id().std_string()] = track;
  base_->video_capturers_[track->id().std_string()] = video_capturer;
}

void FlutterMediaStream::GetSources(std::unique_ptr<MethodResultProxy> result) {
  EncodableList sources;

#ifdef _WIN32
  // На Windows используем IMMDeviceEnumerator (Windows Core Audio) —
  // он гарантированно отдаёт все системные audio-endpoints, в том
  // числе когда `audio_device_->RecordingDevices()` возвращает 0
  // (виртуальные/redirected устройства, ADM не инициализирован до
  // peer connection). После него дописываем ADM-устройства которых
  // в IMM-списке не оказалось (defensive, обычно не срабатывает).
  std::set<std::string> seen_input_ids;
  std::set<std::string> seen_output_ids;
  EnumerateWindowsAudioEndpoints(sources, seen_input_ids, eCapture,
                                  "audioinput");
  EnumerateWindowsAudioEndpoints(sources, seen_output_ids, eRender,
                                  "audiooutput");
#endif

  int nb_audio_devices = base_->audio_device_->RecordingDevices();
  char strNameUTF8[RTCAudioDevice::kAdmMaxDeviceNameSize + 1] = {0};
  char strGuidUTF8[RTCAudioDevice::kAdmMaxGuidSize + 1] = {0};

  for (uint16_t i = 0; i < nb_audio_devices; i++) {
    base_->audio_device_->RecordingDeviceName(i, strNameUTF8, strGuidUTF8);
    std::string device_id =
        SanitizeDeviceIdFromAudioBuffers(strNameUTF8, strGuidUTF8);
#ifdef _WIN32
    if (seen_input_ids.count(device_id)) continue;
#endif
    EncodableMap audio;
    audio[EncodableValue("label")] = EncodableValue(SanitizeLabel(strNameUTF8));
    audio[EncodableValue("deviceId")] = EncodableValue(device_id);
    audio[EncodableValue("facing")] = "";
    audio[EncodableValue("kind")] = "audioinput";
    sources.push_back(EncodableValue(audio));
  }

  nb_audio_devices = base_->audio_device_->PlayoutDevices();
  for (uint16_t i = 0; i < nb_audio_devices; i++) {
    base_->audio_device_->PlayoutDeviceName(i, strNameUTF8, strGuidUTF8);
    std::string device_id =
        SanitizeDeviceIdFromAudioBuffers(strNameUTF8, strGuidUTF8);
#ifdef _WIN32
    if (seen_output_ids.count(device_id)) continue;
#endif
    EncodableMap audio;
    audio[EncodableValue("label")] = EncodableValue(SanitizeLabel(strNameUTF8));
    audio[EncodableValue("deviceId")] = EncodableValue(device_id);
    audio[EncodableValue("facing")] = "";
    audio[EncodableValue("kind")] = "audiooutput";
    sources.push_back(EncodableValue(audio));
  }

  int nb_video_devices = base_->video_device_->NumberOfDevices();
  for (int i = 0; i < nb_video_devices; i++) {
    base_->video_device_->GetDeviceName(i, strNameUTF8, 128, strGuidUTF8, 128);
    EncodableMap video;
    video[EncodableValue("label")] = EncodableValue(SanitizeLabel(strNameUTF8));
    video[EncodableValue("deviceId")] = EncodableValue(
        SanitizeDeviceIdFromVideoBuffers(strNameUTF8, strGuidUTF8));
    video[EncodableValue("facing")] = i == 1 ? "front" : "back";
    video[EncodableValue("kind")] = "videoinput";
    sources.push_back(EncodableValue(video));
  }
  EncodableMap params;
  params[EncodableValue("sources")] = EncodableValue(sources);
  result->Success(EncodableValue(params));
}

// 1-арг форма — обратная совместимость для legacy callers.
// Forwards в 3-арг с пустым label и force_try_set=false.
void FlutterMediaStream::SelectAudioOutput(
    const std::string& device_id,
    std::unique_ptr<MethodResultProxy> result) {
  SelectAudioOutput(device_id, "", false, std::move(result));
}

void FlutterMediaStream::SelectAudioOutput(
    const std::string& device_id,
    const std::string& label,
    bool force_try_set,
    std::unique_ptr<MethodResultProxy> result) {
  // Пустой / "default" deviceId — не пинимся к конкретному устройству,
  // оставляем ADM на системном default'е. Это критично для Windows:
  // selectAudioOutput("default") должен возвращать success, не error,
  // чтобы Dart-слой мог явно сказать «следуй за системой».
  if (device_id == "" || device_id == "default") {
    std::cout << "[FlutterWebRTC] selectAudioOutput: default/empty deviceId"
              << " — skipping (label=\"" << label
              << "\" force=" << force_try_set << ")" << std::endl;
    result->Success();
    return;
  }
  const int playout_devices = base_->audio_device_->PlayoutDevices();
  std::cout << "[FlutterWebRTC] selectAudioOutput requested deviceId="
            << device_id << " label=\"" << label
            << "\" force=" << force_try_set
            << " admDevices=" << playout_devices << std::endl;

  char deviceName[256];
  char deviceGuid[256];
  uint16_t matched_index = 0;
  bool found = false;
  // Primary match — по device_id (sanitized name+guid). На Win совпадает
  // с тем что Dart-side получает через `enumerateDevices`, должен
  // работать out-of-the-box.
  for (uint16_t i = 0; i < playout_devices; i++) {
    base_->audio_device_->PlayoutDeviceName(i, deviceName, deviceGuid);
    std::string cur_device_id =
        SanitizeDeviceIdFromAudioBuffers(deviceName, deviceGuid);
    std::cout << "[FlutterWebRTC]   out candidate i=" << i
              << " id=" << cur_device_id << " name=\"" << deviceName << "\""
              << std::endl;
    if (device_id == cur_device_id) {
      matched_index = i;
      found = true;
      std::cout << "[FlutterWebRTC]   out matched by deviceId → i=" << i
                << std::endl;
      break;
    }
  }
  // Fallback match — по label == ADM-device-name. Полезно если Dart
  // прислал deviceId в формате который Win ADM не использует.
  if (!found && !label.empty()) {
    for (uint16_t i = 0; i < playout_devices; i++) {
      base_->audio_device_->PlayoutDeviceName(i, deviceName, deviceGuid);
      if (label == std::string(deviceName)) {
        matched_index = i;
        found = true;
        std::cout << "[FlutterWebRTC]   out matched by label → i=" << i
                  << " name=\"" << deviceName << "\"" << std::endl;
        break;
      }
    }
  }
  if (!found) {
    // Не нашли в ADM-перечислении — возможно устройство в IMM-списке
    // (Windows Core Audio) но не зарегистрировано в ADM. Тогда
    // success без явной смены — libwebrtc возьмёт системный default.
    std::cout << "[FlutterWebRTC]   out NOT matched, falling back to "
                 "system default"
              << std::endl;
    result->Success();
    return;
  }
  // force_try_set пока не имеет специального обработчика на Win —
  // публичный `RTCAudioDevice` C++ API (libwebrtc v1.4.0) НЕ
  // экспортирует `StopPlayout/InitPlayout/StartPlayout`, поэтому
  // явный hot-swap-цикл во время playout=1 невозможен через текущий
  // API. SetPlayoutDevice применяется немедленно если ADM idle, или
  // lazy на следующий InitPlayout если уже работает. См.
  // `todo_2026_05_17_windows_audio_hot_swap.md` для плана.
  const int32_t rc = base_->audio_device_->SetPlayoutDevice(matched_index);
  std::cout << "[FlutterWebRTC] SetPlayoutDevice(" << matched_index
            << ") rc=" << rc << " (force=" << force_try_set << ")"
            << std::endl;
  result->Success();
}

void FlutterMediaStream::SelectAudioInput(
    const std::string& device_id,
    std::unique_ptr<MethodResultProxy> result) {
  SelectAudioInput(device_id, "", false, std::move(result));
}

void FlutterMediaStream::SelectAudioInput(
    const std::string& device_id,
    const std::string& label,
    bool force_try_set,
    std::unique_ptr<MethodResultProxy> result) {
  if (device_id == "" || device_id == "default") {
    std::cout << "[FlutterWebRTC] selectAudioInput: default/empty deviceId"
              << " — skipping (label=\"" << label
              << "\" force=" << force_try_set << ")" << std::endl;
    result->Success();
    return;
  }
  const int recording_devices = base_->audio_device_->RecordingDevices();
  std::cout << "[FlutterWebRTC] selectAudioInput requested deviceId="
            << device_id << " label=\"" << label
            << "\" force=" << force_try_set
            << " admDevices=" << recording_devices << std::endl;

  char deviceName[256];
  char deviceGuid[256];
  uint16_t matched_index = 0;
  bool found = false;
  for (uint16_t i = 0; i < recording_devices; i++) {
    base_->audio_device_->RecordingDeviceName(i, deviceName, deviceGuid);
    std::string cur_device_id =
        SanitizeDeviceIdFromAudioBuffers(deviceName, deviceGuid);
    std::cout << "[FlutterWebRTC]   in candidate i=" << i
              << " id=" << cur_device_id << " name=\"" << deviceName << "\""
              << std::endl;
    if (device_id == cur_device_id) {
      matched_index = i;
      found = true;
      std::cout << "[FlutterWebRTC]   in matched by deviceId → i=" << i
                << std::endl;
      break;
    }
  }
  if (!found && !label.empty()) {
    for (uint16_t i = 0; i < recording_devices; i++) {
      base_->audio_device_->RecordingDeviceName(i, deviceName, deviceGuid);
      if (label == std::string(deviceName)) {
        matched_index = i;
        found = true;
        std::cout << "[FlutterWebRTC]   in matched by label → i=" << i
                  << " name=\"" << deviceName << "\"" << std::endl;
        break;
      }
    }
  }
  if (!found) {
    std::cout << "[FlutterWebRTC]   in NOT matched, falling back to "
                 "system default"
              << std::endl;
    result->Success();
    return;
  }
  // force_try_set пока no-op — нет публичного API для restart capture
  // (см. SelectAudioOutput выше). SetRecordingDevice применяется
  // немедленно при recording=0, или lazy при recording=1.
  // Если SetRecordingDevice-while-recording окажется идемпотентным с
  // auto-restart внутри WASAPI ADM — hot-swap «бесплатный». Если нет
  // — нужен Step 2/3 из `todo_2026_05_17_windows_audio_hot_swap.md`
  // (force-restart через replaceTrack или OnDeviceChange callback).
  const int32_t rc =
      base_->audio_device_->SetRecordingDevice(matched_index);
  std::cout << "[FlutterWebRTC] SetRecordingDevice(" << matched_index
            << ") rc=" << rc << " (force=" << force_try_set << ")"
            << std::endl;
  result->Success();
}

void FlutterMediaStream::MediaStreamGetTracks(
    const std::string& stream_id,
    std::unique_ptr<MethodResultProxy> result) {
  scoped_refptr<RTCMediaStream> stream = base_->MediaStreamForId(stream_id);

  if (stream) {
    EncodableMap params;
    EncodableList audioTracks;

    auto audio_tracks = stream->audio_tracks();
    for (auto track : audio_tracks.std_vector()) {
      base_->local_tracks_[track->id().std_string()] = track;
      EncodableMap info;
      info[EncodableValue("id")] = EncodableValue(track->id().std_string());
      info[EncodableValue("label")] = EncodableValue(track->id().std_string());
      info[EncodableValue("kind")] = EncodableValue(track->kind().std_string());
      info[EncodableValue("enabled")] = EncodableValue(track->enabled());
      info[EncodableValue("remote")] = EncodableValue(true);
      info[EncodableValue("readyState")] = "live";
      audioTracks.push_back(EncodableValue(info));
    }
    params[EncodableValue("audioTracks")] = EncodableValue(audioTracks);

    EncodableList videoTracks;
    auto video_tracks = stream->video_tracks();
    for (auto track : video_tracks.std_vector()) {
      base_->local_tracks_[track->id().std_string()] = track;
      EncodableMap info;
      info[EncodableValue("id")] = EncodableValue(track->id().std_string());
      info[EncodableValue("label")] = EncodableValue(track->id().std_string());
      info[EncodableValue("kind")] = EncodableValue(track->kind().std_string());
      info[EncodableValue("enabled")] = EncodableValue(track->enabled());
      info[EncodableValue("remote")] = EncodableValue(true);
      info[EncodableValue("readyState")] = "live";
      videoTracks.push_back(EncodableValue(info));
    }

    params[EncodableValue("videoTracks")] = EncodableValue(videoTracks);

    result->Success(EncodableValue(params));
  } else {
    result->Error("MediaStreamGetTracksFailed",
                  "MediaStreamGetTracks() media stream is null !");
  }
}

void FlutterMediaStream::MediaStreamDispose(
    const std::string& stream_id,
    std::unique_ptr<MethodResultProxy> result) {
  scoped_refptr<RTCMediaStream> stream = base_->MediaStreamForId(stream_id);

  if (!stream) {
    result->Error("MediaStreamDisposeFailed",
                  "stream [" + stream_id + "] not found!");
    return;
  }

  vector<scoped_refptr<RTCAudioTrack>> audio_tracks = stream->audio_tracks();

  for (auto track : audio_tracks.std_vector()) {
    stream->RemoveTrack(track);
    base_->local_tracks_.erase(track->id().std_string());
  }

  vector<scoped_refptr<RTCVideoTrack>> video_tracks = stream->video_tracks();
  for (auto track : video_tracks.std_vector()) {
    stream->RemoveTrack(track);
    base_->local_tracks_.erase(track->id().std_string());
    if (base_->video_capturers_.find(track->id().std_string()) !=
        base_->video_capturers_.end()) {
      auto video_capture = base_->video_capturers_[track->id().std_string()];
      if (video_capture->CaptureStarted()) {
        video_capture->StopCapture();
      }
      base_->video_capturers_.erase(track->id().std_string());
    }
  }

  base_->RemoveStreamForId(stream_id);
  result->Success();
}

void FlutterMediaStream::CreateLocalMediaStream(
    std::unique_ptr<MethodResultProxy> result) {
  std::string uuid = base_->GenerateUUID();
  scoped_refptr<RTCMediaStream> stream =
      base_->factory_->CreateStream(uuid.c_str());

  EncodableMap params;
  params[EncodableValue("streamId")] = EncodableValue(uuid);

  base_->local_streams_[uuid] = stream;
  result->Success(EncodableValue(params));
}

void FlutterMediaStream::MediaStreamTrackSetEnable(
    const std::string& track_id,
    std::unique_ptr<MethodResultProxy> result) {
  result->NotImplemented();
}

void FlutterMediaStream::MediaStreamTrackSwitchCamera(
    const std::string& track_id,
    std::unique_ptr<MethodResultProxy> result) {
  result->NotImplemented();
}

void FlutterMediaStream::MediaStreamTrackDispose(
    const std::string& track_id,
    std::unique_ptr<MethodResultProxy> result) {
  for (auto it : base_->local_streams_) {
    auto stream = it.second;
    auto audio_tracks = stream->audio_tracks();
    for (auto track : audio_tracks.std_vector()) {
      if (track->id().std_string() == track_id) {
        stream->RemoveTrack(track);
      }
    }
    auto video_tracks = stream->video_tracks();
    for (auto track : video_tracks.std_vector()) {
      if (track->id().std_string() == track_id) {
        stream->RemoveTrack(track);

        if (base_->video_capturers_.find(track_id) !=
            base_->video_capturers_.end()) {
          auto video_capture = base_->video_capturers_[track_id];
          if (video_capture->CaptureStarted()) {
            video_capture->StopCapture();
          }
          base_->video_capturers_.erase(track_id);
        }
      }
    }
  }
  base_->RemoveMediaTrackForId(track_id);
  result->Success();
}
}  // namespace flutter_webrtc_plugin
