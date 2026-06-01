import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'package:webrtc_interface/webrtc_interface.dart';

import 'media_stream_track_impl.dart';
import 'utils.dart';

/// Structured result from native `selectAudioInput`/`selectAudioOutput`.
///
/// Возвращается из нативного метода вместо `void`. Позволяет Dart-слою
/// понять, реально ли применилось устройство или произошёл silent fallback.
class NativeAudioSelectResult {
  final bool matched;
  /// `deviceId` | `label` | `default` | `not_found`
  final String matchedBy;
  /// ADM playback/recording index, -1 если not matched
  final int appliedIndex;
  /// `SetPlayoutDevice`/`SetRecordingDevice` return code, -1 если не вызывался
  final int rc;
  final String deviceId;
  final String label;
  final bool forceRequested;
  /// human-readable remark (e.g. reason for fallback)
  final String remark;

  const NativeAudioSelectResult({
    this.matched = false,
    this.matchedBy = '',
    this.appliedIndex = -1,
    this.rc = -1,
    this.deviceId = '',
    this.label = '',
    this.forceRequested = false,
    this.remark = '',
  });

  factory NativeAudioSelectResult.fromMap(Map<dynamic, dynamic> map) {
    return NativeAudioSelectResult(
      matched: map['matched'] == true,
      matchedBy: (map['matchedBy'] as String?) ?? '',
      appliedIndex: (map['appliedIndex'] as num?)?.toInt() ?? -1,
      rc: (map['rc'] as num?)?.toInt() ?? -1,
      deviceId: (map['deviceId'] as String?) ?? '',
      label: (map['label'] as String?) ?? '',
      forceRequested: map['forceRequested'] == true,
      remark: (map['remark'] as String?) ?? '',
    );
  }

  @override
  String toString() =>
      'NativeAudioSelectResult(matched=$matched matchedBy=$matchedBy '
      'appliedIndex=$appliedIndex rc=$rc '
      'deviceId=$deviceId label="$label" '
      'force=$forceRequested remark="$remark")';
}

class NativeAudioManagement {
  /// Вызывает native `selectAudioInput` и возвращает structured result.
  ///
  /// Возвращает [NativeAudioSelectResult] — позволяет caller'у понять,
  /// реально ли применилось устройство или native сделал silent fallback
  /// на system default.
  static Future<NativeAudioSelectResult> selectAudioInput(
    String deviceId, {
    String? label,
    bool forceTrySet = false,
  }) async {
    final params = <String, dynamic>{'deviceId': deviceId};
    if (label != null) params['label'] = label;
    if (forceTrySet) params['forceTrySet'] = true;
    final result = await WebRTC.invokeMethod<dynamic, dynamic>(
      'selectAudioInput',
      params,
    );
    if (result is Map) {
      return NativeAudioSelectResult.fromMap(result.cast());
    }
    // Backward-compatible: старые сборки могут вернуть null/void.
    // Тогда возвращаем optimistic результат.
    return NativeAudioSelectResult(
      matched: true,
      matchedBy: 'legacy_void',
      appliedIndex: 0,
      rc: 0,
      deviceId: deviceId,
      label: label ?? '',
      forceRequested: forceTrySet,
      remark: 'old native: no structured result, assumed success',
    );
  }

  /// Вызывает native `selectAudioOutput` и возвращает structured result.
  static Future<NativeAudioSelectResult> selectAudioOutput(
    String deviceId, {
    String? label,
    bool forceTrySet = false,
  }) async {
    final params = <String, dynamic>{'deviceId': deviceId};
    if (label != null) params['label'] = label;
    if (forceTrySet) params['forceTrySet'] = true;
    final result = await WebRTC.invokeMethod<dynamic, dynamic>(
      'selectAudioOutput',
      params,
    );
    if (result is Map) {
      return NativeAudioSelectResult.fromMap(result.cast());
    }
    return NativeAudioSelectResult(
      matched: true,
      matchedBy: 'legacy_void',
      appliedIndex: 0,
      rc: 0,
      deviceId: deviceId,
      label: label ?? '',
      forceRequested: forceTrySet,
      remark: 'old native: no structured result, assumed success',
    );
  }

  static Future<void> setSpeakerphoneOn(bool enable) async {
    await WebRTC.invokeMethod(
      'enableSpeakerphone',
      <String, dynamic>{'enable': enable},
    );
  }

  static Future<void> ensureAudioSession() async {
    await WebRTC.invokeMethod('ensureAudioSession');
  }

  static Future<void> setSpeakerphoneOnButPreferBluetooth() async {
    await WebRTC.invokeMethod('enableSpeakerphoneButPreferBluetooth');
  }

  static Future<void> setVolume(double volume, MediaStreamTrack track) async {
    if (track.kind == 'audio') {
      if (kIsWeb) {
        final constraints = track.getConstraints();
        constraints['volume'] = volume;
        await track.applyConstraints(constraints);
      } else {
        await WebRTC.invokeMethod('setVolume', <String, dynamic>{
          'trackId': track.id,
          'volume': volume,
          'peerConnectionId':
              track is MediaStreamTrackNative ? track.peerConnectionId : null
        });
      }
    }

    return Future.value();
  }

  static Future<void> setMicrophoneMute(
      bool mute, MediaStreamTrack track) async {
    if (track.kind != 'audio') {
      throw 'The is not an audio track => $track';
    }

    if (!kIsWeb) {
      try {
        await WebRTC.invokeMethod(
          'setMicrophoneMute',
          <String, dynamic>{'trackId': track.id, 'mute': mute},
        );
      } on PlatformException catch (e) {
        throw 'Unable to MediaStreamTrack::setMicrophoneMute: ${e.message}';
      }
    }
    track.enabled = !mute;
  }

  // ADM APIs
  static Future<void> startLocalRecording() async {
    if (!kIsWeb) {
      try {
        await WebRTC.invokeMethod(
          'startLocalRecording',
          <String, dynamic>{},
        );
      } on PlatformException catch (e) {
        throw 'Unable to start local recording: ${e.message}';
      }
    }
  }

  static Future<void> stopLocalRecording() async {
    if (!kIsWeb) {
      try {
        await WebRTC.invokeMethod(
          'stopLocalRecording',
          <String, dynamic>{},
        );
      } on PlatformException catch (e) {
        throw 'Unable to stop local recording: ${e.message}';
      }
    }
  }

  static Future<bool> isVoiceProcessingEnabled() async {
    if (kIsWeb) return false;

    try {
      final result = await WebRTC.invokeMethod(
        'isVoiceProcessingEnabled',
        <String, dynamic>{},
      );
      return result as bool;
    } on PlatformException catch (e) {
      throw 'Unable to get isVoiceProcessingEnabled: ${e.message}';
    }
  }

  static Future<bool> isVoiceProcessingBypassed() async {
    if (kIsWeb) return false;

    try {
      final result = await WebRTC.invokeMethod(
        'isVoiceProcessingBypassed',
        <String, dynamic>{},
      );
      return result as bool;
    } on PlatformException catch (e) {
      throw 'Unable to get isVoiceProcessingBypassed: ${e.message}';
    }
  }

  static Future<void> setIsVoiceProcessingBypassed(bool value) async {
    if (kIsWeb) return;

    try {
      await WebRTC.invokeMethod(
        'setIsVoiceProcessingBypassed',
        <String, dynamic>{"value": value},
      );
    } on PlatformException catch (e) {
      throw 'Unable to set isVoiceProcessingBypassed: ${e.message}';
    }
  }
}
