#include "flutter_webrtc/flutter_web_r_t_c_plugin.h"

#include "flutter_common.h"
#include "flutter_webrtc.h"
#include "task_runner_windows.h"
#include "audio_session_naming.h"

#include <flutter/plugin_registrar_windows.h>
#include <string>

const char* kChannelName = "FlutterWebRTC.Method";
static flutter_webrtc_plugin::FlutterWebRTC* g_shared_instance = nullptr;

namespace flutter_webrtc_plugin {

// A webrtc plugin for windows/linux.
class FlutterWebRTCPluginImpl : public FlutterWebRTCPlugin {
 public:
  static void RegisterWithRegistrar(PluginRegistrar* registrar) {
    auto channel = std::make_unique<MethodChannel>(
        registrar->messenger(), kChannelName,
        &flutter::StandardMethodCodec::GetInstance());

    auto* channel_pointer = channel.get();

    // Uses new instead of make_unique due to private constructor.
    std::unique_ptr<FlutterWebRTCPluginImpl> plugin(
        new FlutterWebRTCPluginImpl(registrar, std::move(channel)));
    channel_pointer->SetMethodCallHandler(
        [plugin_pointer = plugin.get()](const auto& call, auto result) {
          plugin_pointer->HandleMethodCall(call, std::move(result));
        });

    registrar->AddPlugin(std::move(plugin));
  }

  virtual ~FlutterWebRTCPluginImpl() {}

  BinaryMessenger* messenger() { return messenger_; }

  TextureRegistrar* textures() { return textures_; }

  TaskRunner* task_runner() { return task_runner_.get(); }

 private:
  // Creates a plugin that communicates on the given channel.
  FlutterWebRTCPluginImpl(PluginRegistrar* registrar,
                          std::unique_ptr<MethodChannel> channel)
      : channel_(std::move(channel)),
        messenger_(registrar->messenger()),
        textures_(registrar->texture_registrar()),
        task_runner_(std::make_unique<TaskRunnerWindows>()) {
    webrtc_ = std::make_unique<FlutterWebRTC>(this);
    g_shared_instance = webrtc_.get();
  }

  // Called when a method is called on |channel_|;
  void HandleMethodCall(const MethodCall& method_call,
                        std::unique_ptr<MethodResult> result) {
    // Win-only расширение: переименование WASAPI-сессий процесса в
    // Volume Mixer. Перехватываем ДО `FlutterWebRTC::HandleMethodCall`
    // — общий код про этот метод не знает.
    if (method_call.method_name().compare("setAudioSessionDisplayName") == 0) {
      std::string utf8_name = "Synergy Call Center";
      if (method_call.arguments() != nullptr &&
          TypeIs<EncodableMap>(*method_call.arguments())) {
        const auto& params = GetValue<EncodableMap>(*method_call.arguments());
        std::string candidate = findString(params, "name");
        if (!candidate.empty()) utf8_name = candidate;
      }
      // UTF-8 → UTF-16 без CRT-locale зависимости. WASAPI принимает
      // только wide-string. На случай чистого ASCII (наш дефолт)
      // конверсия тривиальная, но идентично работает и для кириллицы.
      std::wstring display_name;
      int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_name.c_str(),
                                     static_cast<int>(utf8_name.size()),
                                     nullptr, 0);
      if (wlen > 0) {
        display_name.assign(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8_name.c_str(),
                            static_cast<int>(utf8_name.size()),
                            &display_name[0], wlen);
      } else {
        display_name = L"Synergy Call Center";
      }
      int applied = SetAudioSessionDisplayName(display_name);
      result->Success(EncodableValue(applied));
      return;
    }

    // handle method call and forward to webrtc native sdk.
    auto method_call_proxy = MethodCallProxy::Create(method_call);
    webrtc_->HandleMethodCall(*method_call_proxy.get(),
                              MethodResultProxy::Create(std::move(result)));
  }

 private:
  std::unique_ptr<MethodChannel> channel_;
  std::unique_ptr<FlutterWebRTC> webrtc_;
  BinaryMessenger* messenger_;
  TextureRegistrar* textures_;
  std::unique_ptr<TaskRunner> task_runner_;
};

}  // namespace flutter_webrtc_plugin


void FlutterWebRTCPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  flutter_webrtc_plugin::FlutterWebRTCPluginImpl::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}

flutter_webrtc_plugin::FlutterWebRTC* FlutterWebRTCPluginSharedInstance() {
  return g_shared_instance;
} 