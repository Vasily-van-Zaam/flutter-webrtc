#ifndef FLUTTER_WEBRTC_AUDIO_SESSION_NAMING_H_
#define FLUTTER_WEBRTC_AUDIO_SESSION_NAMING_H_

#include <string>

namespace flutter_webrtc_plugin {

// Назначает `display_name` всем WASAPI-сессиям нашего процесса
// (capture + render, на всех активных IMMDevice'ах). Это то имя,
// которое юзер видит в Windows Volume Mixer / sndvol — без вызова
// здесь libwebrtc открывает сессию с дефолтным именем процесса
// («synergy_call_center»), что выглядит как утечка имени exe.
//
// Можно (и нужно) вызывать **после** того как libwebrtc ADM
// инициализировал свои IAudioClient — иначе сессий ещё нет и
// SetDisplayName не на что назначить. Безопасно вызывать многократно:
// если имя уже стоит — SetDisplayName это no-op.
//
// Параметры:
//   * `display_name` — UTF-16 строка, что показывать в Mixer.
//
// Возвращает число сессий которым присвоено имя (для отладки —
// 0 значит «никаких WASAPI-сессий нашего PID не нашлось», возможно
// слишком ранний вызов).
int SetAudioSessionDisplayName(const std::wstring& display_name);

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_WEBRTC_AUDIO_SESSION_NAMING_H_
