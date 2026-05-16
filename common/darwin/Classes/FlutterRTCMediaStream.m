#import <objc/runtime.h>
#import "AudioUtils.h"
#import "CameraUtils.h"
#import "FlutterRTCFrameCapturer.h"
#import "FlutterRTCMediaStream.h"
#import "FlutterRTCPeerConnection.h"
#import "VideoProcessingAdapter.h"
#import "LocalVideoTrack.h"
#import "LocalAudioTrack.h"
#if TARGET_OS_OSX
#import <CoreAudio/CoreAudio.h>
// `kAudioObjectPropertyElementMain` появилось в macOS 12 SDK. Для
// сборки на более старых SDK используем `kAudioObjectPropertyElementMaster`.
#if !defined(kAudioObjectPropertyElementMain)
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif
#endif

@implementation RTCMediaStreamTrack (Flutter)

- (id)settings {
  return objc_getAssociatedObject(self, _cmd);
}

- (void)setSettings:(id)settings {
  objc_setAssociatedObject(self, @selector(settings), settings, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
@end

@implementation AVCaptureDevice (Flutter)

- (NSString*)positionString {
  switch (self.position) {
    case AVCaptureDevicePositionUnspecified:
      return @"unspecified";
    case AVCaptureDevicePositionBack:
      return @"back";
    case AVCaptureDevicePositionFront:
      return @"front";
  }
  return nil;
}

@end

@implementation FlutterWebRTCPlugin (RTCMediaStream)

/**
 * {@link https://www.w3.org/TR/mediacapture-streams/#navigatorusermediaerrorcallback}
 */
typedef void (^NavigatorUserMediaErrorCallback)(NSString* errorType, NSString* errorMessage);

/**
 * {@link https://www.w3.org/TR/mediacapture-streams/#navigatorusermediasuccesscallback}
 */
typedef void (^NavigatorUserMediaSuccessCallback)(RTCMediaStream* mediaStream);

- (NSDictionary*)defaultVideoConstraints {
    return @{@"minWidth" : @"1280", @"minHeight" : @"720", @"minFrameRate" : @"30"};
}

- (NSDictionary*)defaultAudioConstraints {
    return @{};
}


- (RTCMediaConstraints*)defaultMediaStreamConstraints {
  RTCMediaConstraints* constraints =
      [[RTCMediaConstraints alloc] initWithMandatoryConstraints:[self defaultVideoConstraints]
                                            optionalConstraints:nil];
  return constraints;
}


- (NSArray<AVCaptureDevice*> *) captureDevices {
    if (@available(iOS 13.0, macOS 10.15, macCatalyst 14.0, tvOS 17.0, *)) {
        NSArray<AVCaptureDeviceType> *deviceTypes = @[
#if TARGET_OS_IPHONE
            AVCaptureDeviceTypeBuiltInTripleCamera,
            AVCaptureDeviceTypeBuiltInDualCamera,
            AVCaptureDeviceTypeBuiltInDualWideCamera,
            AVCaptureDeviceTypeBuiltInWideAngleCamera,
            AVCaptureDeviceTypeBuiltInTelephotoCamera,
            AVCaptureDeviceTypeBuiltInUltraWideCamera,
#else
            AVCaptureDeviceTypeBuiltInWideAngleCamera,
#endif
        ];
        
#if !defined(TARGET_OS_IPHONE)
        if (@available(macOS 13.0, *)) {
            deviceTypes = [deviceTypes arrayByAddingObject:AVCaptureDeviceTypeDeskViewCamera];
        }
#endif

        if (@available(iOS 17.0, macOS 14.0, tvOS 17.0, *)) {
            deviceTypes = [deviceTypes arrayByAddingObjectsFromArray: @[
                AVCaptureDeviceTypeContinuityCamera,
                AVCaptureDeviceTypeExternal,
            ]];
        }

        return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:deviceTypes
                                                                      mediaType:AVMediaTypeVideo
                                                                       position:AVCaptureDevicePositionUnspecified].devices;
    }
    return @[];
}

/**
 * Initializes a new {@link RTCAudioTrack} which satisfies specific constraints,
 * adds it to a specific {@link RTCMediaStream}, and reports success to a
 * specific callback. Implements the audio-specific counterpart of the
 * {@code getUserMedia()} algorithm.
 *
 * @param constraints The {@code MediaStreamConstraints} which the new
 * {@code RTCAudioTrack} instance is to satisfy.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is being initialized as
 * part of the execution of the {@code getUserMedia()} algorithm, to which a
 * new {@code RTCAudioTrack} is to be added, and which is to be reported to
 * {@code successCallback} upon success.
 */
- (void)getUserAudio:(NSDictionary*)constraints
     successCallback:(NavigatorUserMediaSuccessCallback)successCallback
       errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
         mediaStream:(RTCMediaStream*)mediaStream {
  id audioConstraints = constraints[@"audio"];
  NSString* audioDeviceId = @"";
  RTCMediaConstraints *rtcConstraints;
  if ([audioConstraints isKindOfClass:[NSDictionary class]]) {
    // constraints.audio.deviceId
    NSString* deviceId = audioConstraints[@"deviceId"];

    if (deviceId) {
      audioDeviceId = deviceId;
    }

    rtcConstraints = [self parseMediaConstraints:audioConstraints];
    // constraints.audio.optional.sourceId
    id optionalConstraints = audioConstraints[@"optional"];
    if (optionalConstraints && [optionalConstraints isKindOfClass:[NSArray class]] &&
        !deviceId) {
      NSArray* options = optionalConstraints;
      for (id item in options) {
        if ([item isKindOfClass:[NSDictionary class]]) {
          NSString* sourceId = ((NSDictionary*)item)[@"sourceId"];
          if (sourceId) {
            audioDeviceId = sourceId;
          }
        }
      }
    }
  } else {
      rtcConstraints = [self parseMediaConstraints:[self defaultAudioConstraints]];
  }

#if !defined(TARGET_OS_IPHONE)
  // ВАЖНО: НЕ вызываем `selectAudioInput:` если deviceId пустой.
  // Иначе ADM ищет совпадение в inputDevices, не находит, идёт по
  // error-path; сам факт обращения к ADM может быть disruptive на
  // cold start. Кроме того, `setInputDevice:` на macOS внутри
  // libwebrtc меняет CoreAudio HAL system-wide default microphone —
  // в Dart-слое мы намеренно не передаём deviceId на macOS, но
  // защищаемся ещё и здесь на случай прихода пустой строки от
  // других callers.
  if (audioDeviceId != nil && audioDeviceId.length > 0) {
    [self selectAudioInput:audioDeviceId result:nil];
  }
#endif

  NSString* trackId = [[NSUUID UUID] UUIDString];
  RTCAudioSource *audioSource = [self.peerConnectionFactory audioSourceWithConstraints:rtcConstraints];
  RTCAudioTrack* audioTrack = [self.peerConnectionFactory audioTrackWithSource:audioSource trackId:trackId];
  LocalAudioTrack *localAudioTrack = [[LocalAudioTrack alloc] initWithTrack:audioTrack];

  audioTrack.settings = @{
    @"deviceId" : audioDeviceId,
    @"kind" : @"audioinput",
    @"autoGainControl" : @YES,
    @"echoCancellation" : @YES,
    @"noiseSuppression" : @YES,
    @"channelCount" : @1,
    @"latency" : @0,
  };

  [mediaStream addAudioTrack:audioTrack];

  [self.localTracks setObject:localAudioTrack forKey:trackId];

  [self ensureAudioSession];

  successCallback(mediaStream);
}

// TODO: Use RCTConvert for constraints ...
- (void)getUserMedia:(NSDictionary*)constraints result:(FlutterResult)result {
  // Initialize RTCMediaStream with a unique label in order to allow multiple
  // RTCMediaStream instances initialized by multiple getUserMedia calls to be
  // added to 1 RTCPeerConnection instance. As suggested by
  // https://www.w3.org/TR/mediacapture-streams/#mediastream to be a good
  // practice, use a UUID (conforming to RFC4122).
  NSString* mediaStreamId = [[NSUUID UUID] UUIDString];
  RTCMediaStream* mediaStream = [self.peerConnectionFactory mediaStreamWithStreamId:mediaStreamId];

  [self getUserMedia:constraints
      successCallback:^(RTCMediaStream* mediaStream) {
        NSString* mediaStreamId = mediaStream.streamId;

        NSMutableArray* audioTracks = [NSMutableArray array];
        NSMutableArray* videoTracks = [NSMutableArray array];

        for (RTCAudioTrack* track in mediaStream.audioTracks) {
          [audioTracks addObject:@{
            @"id" : track.trackId,
            @"kind" : track.kind,
            @"label" : track.trackId,
            @"enabled" : @(track.isEnabled),
            @"remote" : @(YES),
            @"readyState" : @"live",
            @"settings" : track.settings
          }];
        }

        for (RTCVideoTrack* track in mediaStream.videoTracks) {
          [videoTracks addObject:@{
            @"id" : track.trackId,
            @"kind" : track.kind,
            @"label" : track.trackId,
            @"enabled" : @(track.isEnabled),
            @"remote" : @(YES),
            @"readyState" : @"live",
            @"settings" : track.settings
          }];
        }

        self.localStreams[mediaStreamId] = mediaStream;
        result(@{
          @"streamId" : mediaStreamId,
          @"audioTracks" : audioTracks,
          @"videoTracks" : videoTracks
        });
      }
      errorCallback:^(NSString* errorType, NSString* errorMessage) {
        result([FlutterError errorWithCode:[NSString stringWithFormat:@"Error %@", errorType]
                                   message:errorMessage
                                   details:nil]);
      }
      mediaStream:mediaStream];
}

/**
 * Initializes a new {@link RTCAudioTrack} or a new {@link RTCVideoTrack} which
 * satisfies specific constraints and adds it to a specific
 * {@link RTCMediaStream} if the specified {@code mediaStream} contains no track
 * of the respective media type and the specified {@code constraints} specify
 * that a track of the respective media type is required; otherwise, reports
 * success for the specified {@code mediaStream} to a specific
 * {@link NavigatorUserMediaSuccessCallback}. In other words, implements a media
 * type-specific iteration of or successfully concludes the
 * {@code getUserMedia()} algorithm. The method will be recursively invoked to
 * conclude the whole {@code getUserMedia()} algorithm either with (successful)
 * satisfaction of the specified {@code constraints} or with failure.
 *
 * @param constraints The {@code MediaStreamConstraints} which specifies the
 * requested media types and which the new {@code RTCAudioTrack} or
 * {@code RTCVideoTrack} instance is to satisfy.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is being initialized as
 * part of the execution of the {@code getUserMedia()} algorithm.
 */
- (void)getUserMedia:(NSDictionary*)constraints
     successCallback:(NavigatorUserMediaSuccessCallback)successCallback
       errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
         mediaStream:(RTCMediaStream*)mediaStream {
  // If mediaStream contains no audioTracks and the constraints request such a
  // track, then run an iteration of the getUserMedia() algorithm to obtain
  // local audio content.
  if (mediaStream.audioTracks.count == 0) {
    // constraints.audio
    id audioConstraints = constraints[@"audio"];
    BOOL constraintsIsDictionary = [audioConstraints isKindOfClass:[NSDictionary class]];
    if (audioConstraints && (constraintsIsDictionary || [audioConstraints boolValue])) {
      [self requestAccessForMediaType:AVMediaTypeAudio
                          constraints:constraints
                      successCallback:successCallback
                        errorCallback:errorCallback
                          mediaStream:mediaStream];
      return;
    }
  }

  // If mediaStream contains no videoTracks and the constraints request such a
  // track, then run an iteration of the getUserMedia() algorithm to obtain
  // local video content.
  if (mediaStream.videoTracks.count == 0) {
    // constraints.video
    id videoConstraints = constraints[@"video"];
    if (videoConstraints) {
      BOOL requestAccessForVideo = [videoConstraints isKindOfClass:[NSNumber class]]
                                       ? [videoConstraints boolValue]
                                       : [videoConstraints isKindOfClass:[NSDictionary class]];
#if !TARGET_IPHONE_SIMULATOR
      if (requestAccessForVideo) {
        [self requestAccessForMediaType:AVMediaTypeVideo
                            constraints:constraints
                        successCallback:successCallback
                          errorCallback:errorCallback
                            mediaStream:mediaStream];
        return;
      }
#endif
    }
  }

  // There are audioTracks and/or videoTracks in mediaStream as requested by
  // constraints so the getUserMedia() is to conclude with success.
  successCallback(mediaStream);
}

- (int)getConstrainInt:(NSDictionary*)constraints forKey:(NSString*)key {
  if (![constraints isKindOfClass:[NSDictionary class]]) {
    return 0;
  }

  id constraint = constraints[key];
  if ([constraint isKindOfClass:[NSNumber class]]) {
    return [constraint intValue];
  } else if ([constraint isKindOfClass:[NSString class]]) {
    int possibleValue = [constraint intValue];
    if (possibleValue != 0) {
      return possibleValue;
    }
  } else if ([constraint isKindOfClass:[NSDictionary class]]) {
    id idealConstraint = constraint[@"ideal"];
    if ([idealConstraint isKindOfClass:[NSString class]]) {
      int possibleValue = [idealConstraint intValue];
      if (possibleValue != 0) {
        return possibleValue;
      }
    }
  }

  return 0;
}

/**
 * Initializes a new {@link RTCVideoTrack} which satisfies specific constraints,
 * adds it to a specific {@link RTCMediaStream}, and reports success to a
 * specific callback. Implements the video-specific counterpart of the
 * {@code getUserMedia()} algorithm.
 *
 * @param constraints The {@code MediaStreamConstraints} which the new
 * {@code RTCVideoTrack} instance is to satisfy.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is being initialized as
 * part of the execution of the {@code getUserMedia()} algorithm, to which a
 * new {@code RTCVideoTrack} is to be added, and which is to be reported to
 * {@code successCallback} upon success.
 */
- (void)getUserVideo:(NSDictionary*)constraints
     successCallback:(NavigatorUserMediaSuccessCallback)successCallback
       errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
         mediaStream:(RTCMediaStream*)mediaStream {
  id videoConstraints = constraints[@"video"];
  AVCaptureDevice* videoDevice;
  NSString* videoDeviceId = nil;
  NSString* facingMode = nil;
  NSArray<AVCaptureDevice*>* captureDevices = [self captureDevices];

  if ([videoConstraints isKindOfClass:[NSDictionary class]]) {
    // constraints.video.deviceId
    NSString* deviceId = videoConstraints[@"deviceId"];

    if (deviceId) {
        for (AVCaptureDevice *device in captureDevices) {
            if( [deviceId isEqualToString:device.uniqueID]) {
                videoDevice = device;
                videoDeviceId = deviceId;
            }
        }
    }

    // constraints.video.optional
    id optionalVideoConstraints = videoConstraints[@"optional"];
    if (optionalVideoConstraints && [optionalVideoConstraints isKindOfClass:[NSArray class]] &&
        !videoDevice) {
      NSArray* options = optionalVideoConstraints;
      for (id item in options) {
        if ([item isKindOfClass:[NSDictionary class]]) {
          NSString* sourceId = ((NSDictionary*)item)[@"sourceId"];
          if (sourceId) {
              for (AVCaptureDevice *device in captureDevices) {
                  if( [sourceId isEqualToString:device.uniqueID]) {
                      videoDevice = device;
                      videoDeviceId = sourceId;
                  }
              }
            if (videoDevice) {
              break;
            }
          }
        }
      }
    }

    if (!videoDevice) {
      // constraints.video.facingMode
      // https://www.w3.org/TR/mediacapture-streams/#def-constraint-facingMode
      facingMode = videoConstraints[@"facingMode"];
      if (facingMode && [facingMode isKindOfClass:[NSString class]]) {
        AVCaptureDevicePosition position;
        if ([facingMode isEqualToString:@"environment"]) {
          self._usingFrontCamera = NO;
          position = AVCaptureDevicePositionBack;
        } else if ([facingMode isEqualToString:@"user"]) {
          self._usingFrontCamera = YES;
          position = AVCaptureDevicePositionFront;
        } else {
          // If the specified facingMode value is not supported, fall back to
          // the default video device.
          self._usingFrontCamera = NO;
          position = AVCaptureDevicePositionUnspecified;
        }
        videoDevice = [self findDeviceForPosition:position];
      }
    }
  }

  if ([videoConstraints isKindOfClass:[NSNumber class]]) {
    videoConstraints = @{@"mandatory": [self defaultVideoConstraints]};
  }

  NSInteger targetWidth = 0;
  NSInteger targetHeight = 0;
  NSInteger targetFps = 0;

  if (!videoDevice) {
    videoDevice = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  }

  int possibleWidth = [self getConstrainInt:videoConstraints forKey:@"width"];
  if (possibleWidth != 0) {
    targetWidth = possibleWidth;
  }

  int possibleHeight = [self getConstrainInt:videoConstraints forKey:@"height"];
  if (possibleHeight != 0) {
    targetHeight = possibleHeight;
  }

  int possibleFps = [self getConstrainInt:videoConstraints forKey:@"frameRate"];
  if (possibleFps != 0) {
    targetFps = possibleFps;
  }

  id mandatory =
      [videoConstraints isKindOfClass:[NSDictionary class]] ? videoConstraints[@"mandatory"] : nil;

  // constraints.video.mandatory
  if (mandatory && [mandatory isKindOfClass:[NSDictionary class]]) {
    id widthConstraint = mandatory[@"minWidth"];
    if ([widthConstraint isKindOfClass:[NSString class]] ||
        [widthConstraint isKindOfClass:[NSNumber class]]) {
      int possibleWidth = [widthConstraint intValue];
      if (possibleWidth != 0) {
        targetWidth = possibleWidth;
      }
    }
    id heightConstraint = mandatory[@"minHeight"];
    if ([heightConstraint isKindOfClass:[NSString class]] ||
        [heightConstraint isKindOfClass:[NSNumber class]]) {
      int possibleHeight = [heightConstraint intValue];
      if (possibleHeight != 0) {
        targetHeight = possibleHeight;
      }
    }
    id fpsConstraint = mandatory[@"minFrameRate"];
    if ([fpsConstraint isKindOfClass:[NSString class]] ||
        [fpsConstraint isKindOfClass:[NSNumber class]]) {
      int possibleFps = [fpsConstraint intValue];
      if (possibleFps != 0) {
        targetFps = possibleFps;
      }
    }
  }

  if (videoDevice) {
    RTCVideoSource* videoSource = [self.peerConnectionFactory videoSource];
#if TARGET_OS_OSX
    if (self.videoCapturer) {
      [self.videoCapturer stopCapture];
    }
#endif
      
    VideoProcessingAdapter *videoProcessingAdapter = [[VideoProcessingAdapter alloc] initWithRTCVideoSource:videoSource];
    self.videoCapturer = [[RTCCameraVideoCapturer alloc] initWithDelegate:videoProcessingAdapter];
      
    AVCaptureDeviceFormat* selectedFormat = [self selectFormatForDevice:videoDevice
                                                            targetWidth:targetWidth
                                                           targetHeight:targetHeight];

    CMVideoDimensions selectedDimension = CMVideoFormatDescriptionGetDimensions(selectedFormat.formatDescription);
    NSInteger selectedWidth = (NSInteger) selectedDimension.width;
    NSInteger selectedHeight = (NSInteger) selectedDimension.height;
    NSInteger selectedFps = [self selectFpsForFormat:selectedFormat targetFps:targetFps];

    self._lastTargetFps = selectedFps;
    self._lastTargetWidth = targetWidth;
    self._lastTargetHeight = targetHeight;
    
    NSLog(@"target format %ldx%ld, targetFps: %ld, selected format: %ldx%ld, selected fps %ld", targetWidth, targetHeight, targetFps, selectedWidth, selectedHeight, selectedFps);

    if ([videoDevice lockForConfiguration:NULL]) {
      @try {
        videoDevice.activeVideoMaxFrameDuration = CMTimeMake(1, (int32_t)selectedFps);
        videoDevice.activeVideoMinFrameDuration = CMTimeMake(1, (int32_t)selectedFps);
      } @catch (NSException* exception) {
        NSLog(@"Failed to set active frame rate!\n User info:%@", exception.userInfo);
      }
      [videoDevice unlockForConfiguration];
    }

    [self.videoCapturer startCaptureWithDevice:videoDevice
                                        format:selectedFormat
                                           fps:selectedFps
                             completionHandler:^(NSError* error) {
                               if (error) {
                                 NSLog(@"Start capture error: %@", [error localizedDescription]);
                               }
                             }];

    NSString* trackUUID = [[NSUUID UUID] UUIDString];
    RTCVideoTrack* videoTrack = [self.peerConnectionFactory videoTrackWithSource:videoSource
                                                                        trackId:trackUUID];
    LocalVideoTrack *localVideoTrack = [[LocalVideoTrack alloc] initWithTrack:videoTrack videoProcessing:videoProcessingAdapter];
      
    __weak RTCCameraVideoCapturer* capturer = self.videoCapturer;
    self.videoCapturerStopHandlers[videoTrack.trackId] = ^(CompletionHandler handler) {
      NSLog(@"Stop video capturer, trackID %@", videoTrack.trackId);
      [capturer stopCaptureWithCompletionHandler:handler];
    };

    if (!videoDeviceId) {
      videoDeviceId = videoDevice.uniqueID;
    }

    if (!facingMode) {
      facingMode = videoDevice.position == AVCaptureDevicePositionBack    ? @"environment"
                   : videoDevice.position == AVCaptureDevicePositionFront ? @"user"
                                                                          : @"unspecified";
    }

    videoTrack.settings = @{
      @"deviceId" : videoDeviceId,
      @"kind" : @"videoinput",
      @"width" : [NSNumber numberWithInteger:selectedWidth],
      @"height" : [NSNumber numberWithInteger:selectedHeight],
      @"frameRate" : [NSNumber numberWithInteger:selectedFps],
      @"facingMode" : facingMode,
    };

    [mediaStream addVideoTrack:videoTrack];

    [self.localTracks setObject:localVideoTrack forKey:trackUUID];

    successCallback(mediaStream);
  } else {
    // According to step 6.2.3 of the getUserMedia() algorithm, if there is no
    // source, fail with a new OverconstrainedError.
    errorCallback(@"OverconstrainedError", /* errorMessage */ nil);
  }
}

- (void)mediaStreamRelease:(RTCMediaStream*)stream {
  if (stream) {
    for (RTCVideoTrack* track in stream.videoTracks) {
      [self.localTracks removeObjectForKey:track.trackId];
    }
    for (RTCAudioTrack* track in stream.audioTracks) {
      [self.localTracks removeObjectForKey:track.trackId];
    }
    [self.localStreams removeObjectForKey:stream.streamId];
  }
}

/**
 * Obtains local media content of a specific type. Requests access for the
 * specified {@code mediaType} if necessary. In other words, implements a media
 * type-specific iteration of the {@code getUserMedia()} algorithm.
 *
 * @param mediaType Either {@link AVMediaTypAudio} or {@link AVMediaTypeVideo}
 * which specifies the type of the local media content to obtain.
 * @param constraints The {@code MediaStreamConstraints} which are to be
 * satisfied by the obtained local media content.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is to collect the
 * obtained local media content of the specified {@code mediaType}.
 */
- (void)requestAccessForMediaType:(NSString*)mediaType
                      constraints:(NSDictionary*)constraints
                  successCallback:(NavigatorUserMediaSuccessCallback)successCallback
                    errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
                      mediaStream:(RTCMediaStream*)mediaStream {
  // According to step 6.2.1 of the getUserMedia() algorithm, if there is no
  // source, fail "with a new DOMException object whose name attribute has the
  // value NotFoundError."
  // XXX The following approach does not work for audio in Simulator. That is
  // because audio capture is done using AVAudioSession which does not use
  // AVCaptureDevice there. Anyway, Simulator will not (visually) request access
  // for audio.
  if (mediaType == AVMediaTypeVideo && [self captureDevices].count == 0) {
    // Since successCallback and errorCallback are asynchronously invoked
    // elsewhere, make sure that the invocation here is consistent.
    dispatch_async(dispatch_get_main_queue(), ^{
      errorCallback(@"DOMException", @"NotFoundError");
    });
    return;
  }

#if TARGET_OS_OSX
  if (@available(macOS 10.14, *)) {
#endif
    [AVCaptureDevice requestAccessForMediaType:mediaType
                             completionHandler:^(BOOL granted) {
                               dispatch_async(dispatch_get_main_queue(), ^{
                                 if (granted) {
                                   NavigatorUserMediaSuccessCallback scb =
                                       ^(RTCMediaStream* mediaStream) {
                                         [self getUserMedia:constraints
                                             successCallback:successCallback
                                               errorCallback:errorCallback
                                                 mediaStream:mediaStream];
                                       };

                                   if (mediaType == AVMediaTypeAudio) {
                                     [self getUserAudio:constraints
                                         successCallback:scb
                                           errorCallback:errorCallback
                                             mediaStream:mediaStream];
                                   } else if (mediaType == AVMediaTypeVideo) {
                                     [self getUserVideo:constraints
                                         successCallback:scb
                                           errorCallback:errorCallback
                                             mediaStream:mediaStream];
                                   }
                                 } else {
                                   // According to step 10 Permission Failure of the getUserMedia()
                                   // algorithm, if the user has denied permission, fail "with a new
                                   // DOMException object whose name attribute has the value
                                   // NotAllowedError."
                                   errorCallback(@"DOMException", @"NotAllowedError");
                                 }
                               });
                             }];
#if TARGET_OS_OSX
  } else {
    // Fallback on earlier versions
    NavigatorUserMediaSuccessCallback scb = ^(RTCMediaStream* mediaStream) {
      [self getUserMedia:constraints
          successCallback:successCallback
            errorCallback:errorCallback
              mediaStream:mediaStream];
    };
    if (mediaType == AVMediaTypeAudio) {
      [self getUserAudio:constraints
          successCallback:scb
            errorCallback:errorCallback
              mediaStream:mediaStream];
    } else if (mediaType == AVMediaTypeVideo) {
      [self getUserVideo:constraints
          successCallback:scb
            errorCallback:errorCallback
              mediaStream:mediaStream];
    }
  }
#endif
}

- (void)createLocalMediaStream:(FlutterResult)result {
  NSString* mediaStreamId = [[NSUUID UUID] UUIDString];
  RTCMediaStream* mediaStream = [self.peerConnectionFactory mediaStreamWithStreamId:mediaStreamId];

  self.localStreams[mediaStreamId] = mediaStream;
  result(@{@"streamId" : [mediaStream streamId]});
}

#if TARGET_OS_OSX
/// Перечислить audio-устройства указанного scope (input/output) через
/// CoreAudio HAL и добавить их в `sources`. Используется как fallback
/// когда `RTCAudioDeviceModule` не отдаёт devices (cold start, ADM
/// ленивая инициализация). HAL видит ВСЕ системные audio-устройства
/// независимо от текущего state AVFoundation, включая Bluetooth-
/// гарнитуры в A2DP-режиме (которые `AVCaptureDevice` пропускает).
- (void)enumerateMacOSCoreAudioDevicesIntoSources:(NSMutableArray*)sources
                                            scope:(AudioObjectPropertyScope)scope
                                             kind:(NSString*)kind {
  AudioObjectPropertyAddress devListAddr = {
    kAudioHardwarePropertyDevices,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
  };
  UInt32 dataSize = 0;
  OSStatus status = AudioObjectGetPropertyDataSize(
      kAudioObjectSystemObject, &devListAddr, 0, NULL, &dataSize);
  if (status != noErr || dataSize == 0) return;

  UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
  AudioDeviceID* deviceIDs = (AudioDeviceID*)malloc(dataSize);
  status = AudioObjectGetPropertyData(
      kAudioObjectSystemObject, &devListAddr, 0, NULL,
      &dataSize, deviceIDs);
  if (status != noErr) {
    free(deviceIDs);
    return;
  }

  for (UInt32 i = 0; i < deviceCount; i++) {
    AudioDeviceID deviceID = deviceIDs[i];

    // Считаем количество каналов в нужном scope (input/output).
    AudioObjectPropertyAddress streamAddr = {
      kAudioDevicePropertyStreamConfiguration,
      scope,
      kAudioObjectPropertyElementMain
    };
    UInt32 streamSize = 0;
    UInt32 channelCount = 0;
    if (AudioObjectGetPropertyDataSize(
            deviceID, &streamAddr, 0, NULL, &streamSize) == noErr
        && streamSize > 0) {
      AudioBufferList* bufferList = (AudioBufferList*)malloc(streamSize);
      if (AudioObjectGetPropertyData(
              deviceID, &streamAddr, 0, NULL, &streamSize, bufferList)
              == noErr) {
        for (UInt32 b = 0; b < bufferList->mNumberBuffers; b++) {
          channelCount += bufferList->mBuffers[b].mNumberChannels;
        }
      }
      free(bufferList);
    }

    // Bluetooth-гарнитуры (AirPods и т.п.) могут показывать 0 input
    // channels пока они в A2DP-профиле (только output / стерео-музыка).
    // При запросе mic'а через getUserMedia macOS автоматически
    // переключит их в HFP/HSP с микрофоном. Поэтому для Bluetooth-
    // устройств добавляем их в input список даже когда channelCount=0,
    // если у них есть output channels (== доказательство что устройство
    // вообще существует и работает).
    BOOL isBluetooth = NO;
    if (channelCount == 0 &&
        scope == kAudioDevicePropertyScopeInput) {
      AudioObjectPropertyAddress transportAddr = {
        kAudioDevicePropertyTransportType,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      UInt32 transportType = 0;
      UInt32 transportSize = sizeof(transportType);
      if (AudioObjectGetPropertyData(deviceID, &transportAddr, 0, NULL,
                                     &transportSize, &transportType) == noErr
          && transportType == kAudioDeviceTransportTypeBluetooth) {
        // Проверяем, что у Bluetooth-устройства есть output channels —
        // т.е. это headset/гарнитура, а не сторонний неподключённый
        // device record.
        AudioObjectPropertyAddress outAddr = {
          kAudioDevicePropertyStreamConfiguration,
          kAudioDevicePropertyScopeOutput,
          kAudioObjectPropertyElementMain
        };
        UInt32 outSize = 0;
        UInt32 outChannels = 0;
        if (AudioObjectGetPropertyDataSize(
                deviceID, &outAddr, 0, NULL, &outSize) == noErr
            && outSize > 0) {
          AudioBufferList* outBuf = (AudioBufferList*)malloc(outSize);
          if (AudioObjectGetPropertyData(
                  deviceID, &outAddr, 0, NULL, &outSize, outBuf) == noErr) {
            for (UInt32 b = 0; b < outBuf->mNumberBuffers; b++) {
              outChannels += outBuf->mBuffers[b].mNumberChannels;
            }
          }
          free(outBuf);
        }
        if (outChannels > 0) {
          isBluetooth = YES;
        }
      }
    }
    if (channelCount == 0 && !isBluetooth) continue;

    // Имя и UID устройства.
    CFStringRef nameRef = NULL;
    UInt32 nameSize = sizeof(nameRef);
    AudioObjectPropertyAddress nameAddr = {
      kAudioObjectPropertyName,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain
    };
    NSString* name = nil;
    if (AudioObjectGetPropertyData(deviceID, &nameAddr, 0, NULL,
                                   &nameSize, &nameRef) == noErr
        && nameRef != NULL) {
      name = (__bridge_transfer NSString*)nameRef;
    }

    CFStringRef uidRef = NULL;
    UInt32 uidSize = sizeof(uidRef);
    AudioObjectPropertyAddress uidAddr = {
      kAudioDevicePropertyDeviceUID,
      kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain
    };
    NSString* uid = nil;
    if (AudioObjectGetPropertyData(deviceID, &uidAddr, 0, NULL,
                                   &uidSize, &uidRef) == noErr
        && uidRef != NULL) {
      uid = (__bridge_transfer NSString*)uidRef;
    }

    [sources addObject:@{
      @"deviceId" : uid ?: [NSString stringWithFormat:@"%u", (unsigned)deviceID],
      @"label" : name ?: @"",
      @"kind" : kind,
    }];
  }
  free(deviceIDs);
}
#endif

- (void)getSources:(FlutterResult)result {
  NSMutableArray* sources = [NSMutableArray array];
  NSArray* videoDevices =  [self captureDevices];
  for (AVCaptureDevice* device in videoDevices) {
    [sources addObject:@{
      @"facing" : device.positionString,
      @"deviceId" : device.uniqueID,
      @"label" : device.localizedName,
      @"kind" : @"videoinput",
    }];
  }
#if TARGET_OS_IPHONE

  RTCAudioSession* session = [RTCAudioSession sharedInstance];
  for (AVAudioSessionPortDescription* port in session.session.availableInputs) {
    // NSLog(@"input portName: %@, type %@", port.portName,port.portType);
    [sources addObject:@{
      @"deviceId" : port.UID,
      @"label" : port.portName,
      @"groupId" : port.portType,
      @"kind" : @"audioinput",
    }];
  }

  for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
    // NSLog(@"output portName: %@, type %@", port.portName,port.portType);
    if (session.currentRoute.outputs.count == 1 && ![port.UID isEqualToString:@"Speaker"]) {
      [sources addObject:@{
        @"deviceId" : @"Speaker",
        @"label" : @"Speaker",
        @"groupId" : @"Speaker",
        @"kind" : @"audiooutput",
      }];
    }
    [sources addObject:@{
      @"deviceId" : port.UID,
      @"label" : port.portName,
      @"groupId" : port.portType,
      @"kind" : @"audiooutput",
    }];
  }
#endif
#if TARGET_OS_OSX
  RTCAudioDeviceModule* audioDeviceModule = [self.peerConnectionFactory audioDeviceModule];

  // На macOS CoreAudio HAL — наиболее полный источник info об audio
  // устройствах. ADM (`audioDeviceModule.inputDevices/outputDevices`)
  // часто пуст до peer connection init и пропускает Bluetooth-
  // гарнитуры в A2DP-режиме. Поэтому всегда используем HAL для
  // основного списка, а ADM-результаты добавляем сверху как
  // дополнительные (вдруг там есть устройства которых HAL не показал).
  [self enumerateMacOSCoreAudioDevicesIntoSources:sources
                                            scope:kAudioDevicePropertyScopeInput
                                             kind:@"audioinput"];
  [self enumerateMacOSCoreAudioDevicesIntoSources:sources
                                            scope:kAudioDevicePropertyScopeOutput
                                             kind:@"audiooutput"];

  // Дописываем ADM-устройства которые HAL пропустил.
  //
  // ВАЖНО: ADM использует свои внутренние числовые id ("147", "153"),
  // а HAL — CoreAudio UID ("40-B3-FA-74-7F-22:input"). Для одного и
  // того же физического устройства эти id РАЗНЫЕ — поэтому дедуп по
  // `deviceId` оставляет дубли. Дедупим по нормализованному label.
  //
  // Также пропускаем ADM-aliases с label "Default" / "default - ..." —
  // это синонимы default-устройства, а не отдельные единицы железа.
  // Они появляются после первой инициализации peer connection.
  NSMutableSet<NSString*>* seenInputLabels = [NSMutableSet set];
  NSMutableSet<NSString*>* seenOutputLabels = [NSMutableSet set];
  NSCharacterSet* trimSet = [NSCharacterSet whitespaceAndNewlineCharacterSet];
  for (NSDictionary* d in sources) {
    NSString* kind = d[@"kind"];
    NSString* label = d[@"label"] ?: @"";
    label = [[label stringByTrimmingCharactersInSet:trimSet] lowercaseString];
    if (label.length == 0) continue;
    if ([kind isEqualToString:@"audioinput"]) [seenInputLabels addObject:label];
    else if ([kind isEqualToString:@"audiooutput"]) [seenOutputLabels addObject:label];
  }
  for (RTCIODevice* device in [audioDeviceModule inputDevices]) {
    NSString* normLabel =
        [[(device.name ?: @"") stringByTrimmingCharactersInSet:trimSet] lowercaseString];
    if (normLabel.length == 0) continue;
    if ([normLabel hasPrefix:@"default"]) continue;
    if ([seenInputLabels containsObject:normLabel]) continue;
    [seenInputLabels addObject:normLabel];
    [sources addObject:@{
      @"deviceId" : device.deviceId,
      @"label" : device.name,
      @"kind" : @"audioinput",
    }];
  }
  for (RTCIODevice* device in [audioDeviceModule outputDevices]) {
    NSString* normLabel =
        [[(device.name ?: @"") stringByTrimmingCharactersInSet:trimSet] lowercaseString];
    if (normLabel.length == 0) continue;
    if ([normLabel hasPrefix:@"default"]) continue;
    if ([seenOutputLabels containsObject:normLabel]) continue;
    [seenOutputLabels addObject:normLabel];
    [sources addObject:@{
      @"deviceId" : device.deviceId,
      @"label" : device.name,
      @"kind" : @"audiooutput",
    }];
  }
#endif
  result(@{@"sources" : sources});
}

- (void)selectAudioInput:(NSString*)deviceId result:(FlutterResult)result {
  [self selectAudioInput:deviceId label:nil forceTrySet:NO result:result];
}

- (void)selectAudioInput:(NSString*)deviceId
                   label:(NSString*)label
                  result:(FlutterResult)result {
  [self selectAudioInput:deviceId label:label forceTrySet:NO result:result];
}

- (void)selectAudioInput:(NSString*)deviceId
                   label:(NSString*)label
             forceTrySet:(BOOL)forceTrySet
                  result:(FlutterResult)result {
#if TARGET_OS_OSX
  RTCAudioDeviceModule* audioDeviceModule = [self.peerConnectionFactory audioDeviceModule];
  NSArray* inputDevices = [audioDeviceModule inputDevices];
  NSLog(@"[FlutterWebRTC] selectAudioInput requested deviceId=%@ label=%@ available=%lu recording=%d force=%d",
        deviceId, label, (unsigned long)inputDevices.count, audioDeviceModule.recording, forceTrySet);
  RTCIODevice* matched = nil;
  // Primary match: deviceId. На macOS обычно НЕ работает — Dart-side
  // enumerate использует CoreAudio HAL UID (например "BuiltInMicrophoneDevice"),
  // а ADM.inputDevices.deviceId — индексы (например "72"). Оставлено для
  // совместимости с тем, кто всё-таки передаёт ADM-id напрямую.
  for (RTCIODevice* device in inputDevices) {
    NSLog(@"[FlutterWebRTC]   in candidate id=%@ name=%@", device.deviceId, device.name);
    if ([deviceId isEqualToString:device.deviceId]) {
      matched = device;
      break;
    }
  }
  // Fallback: точное совпадение по name == label. Этим путём идёт
  // SCC: лейбл устройства из enumerate'а одинаково отображается и в
  // ADM (RTCIODevice.name), и на dart-стороне (MediaDeviceInfo.label).
  if (matched == nil && label.length > 0) {
    for (RTCIODevice* device in inputDevices) {
      if ([label isEqualToString:device.name]) {
        matched = device;
        NSLog(@"[FlutterWebRTC]   in matched by label → id=%@ name=%@", device.deviceId, device.name);
        break;
      }
    }
  }
  if (matched != nil) {
    // Логика симметрична `selectAudioOutput`:
    //
    //   * `recording=0` (ADM idle, между звонками) → `trySetInputDevice`
    //     безопасен, обновляет inputDevice и переинициализирует capture
    //     на следующий start.
    //
    //   * `recording=1 && forceTrySet=0` → lazy property set. Новый
    //     inputDevice применится только при следующем capture init
    //     (т.е. при следующем `getUserMedia` после `stopRecording`).
    //     В этом режиме `getUserMedia` поверх активного capture **не
    //     пересоздаёт** AudioCaptureClient — track связан со старым
    //     устройством. Это и есть симптом «горячая замена работает
    //     только со следующим звонком».
    //
    //   * `recording=1 && forceTrySet=1` → полный stop → set → init →
    //     start цикл (как для output). audio_unit пересоздаётся с новым
    //     CoreAudio device id; следующий `getUserMedia` (вызывается
    //     из `_refreshActiveCallAudioTrack` на Dart-стороне) создаст
    //     track из НОВОГО capture, `replaceTrack` на active sender
    //     переключит микрофон без re-INVITE. Glitch в разговоре ~50-150 мс.
    if (audioDeviceModule.recording && !forceTrySet) {
      audioDeviceModule.inputDevice = matched;
      NSLog(@"[FlutterWebRTC] setInputDevice (lazy, recording=1) → %@", matched.name);
    } else if (audioDeviceModule.recording && forceTrySet) {
      NSInteger stopRc = [audioDeviceModule stopRecording];
      audioDeviceModule.inputDevice = matched;
      NSInteger initRc = [audioDeviceModule initRecording];
      NSInteger startRc = [audioDeviceModule startRecording];
      NSLog(@"[FlutterWebRTC] hot-swap input (force=1, recording=1): stop=%ld → set=%@ → init=%ld → start=%ld",
            (long)stopRc, matched.name, (long)initRc, (long)startRc);
    } else {
      BOOL ok = [audioDeviceModule trySetInputDevice:matched];
      NSLog(@"[FlutterWebRTC] trySetInputDevice → %d (device=%@)", ok, matched.name);
    }
    self.pendingInputLabel = nil;
    if (result)
      result(nil);
    return;
  }
  // Match не нашёлся: ADM ещё пустой (раннее apply, до peer connection)
  // или device пропал. Сохраняем label как pending — применим в
  // `audioDeviceModuleDidUpdateDevices:` когда ADM проенумерируется.
  if (label.length > 0) {
    self.pendingInputLabel = label;
    NSLog(@"[FlutterWebRTC] selectAudioInput: stored pending label=%@", label);
    if (result)
      result(nil);
    return;
  }
#endif
#if TARGET_OS_IPHONE
  RTCAudioSession* session = [RTCAudioSession sharedInstance];
  for (AVAudioSessionPortDescription* port in session.session.availableInputs) {
    if ([port.UID isEqualToString:deviceId]) {
      if (self.preferredInput != port.portType) {
        self.preferredInput = port.portType;
        [AudioUtils selectAudioInput:self.preferredInput];
      }
      break;
    }
  }
  if (result)
    result(nil);
#endif
  if (result)
    result([FlutterError errorWithCode:@"selectAudioInputFailed"
                               message:[NSString stringWithFormat:@"Error: deviceId not found!"]
                               details:nil]);
}

- (void)selectAudioOutput:(NSString*)deviceId result:(FlutterResult)result {
  [self selectAudioOutput:deviceId label:nil forceTrySet:NO result:result];
}

- (void)selectAudioOutput:(NSString*)deviceId
                    label:(NSString*)label
                   result:(FlutterResult)result {
  [self selectAudioOutput:deviceId label:label forceTrySet:NO result:result];
}

- (void)selectAudioOutput:(NSString*)deviceId
                    label:(NSString*)label
             forceTrySet:(BOOL)forceTrySet
                   result:(FlutterResult)result {
#if TARGET_OS_OSX
  RTCAudioDeviceModule* audioDeviceModule = [self.peerConnectionFactory audioDeviceModule];
  NSArray* outputDevices = [audioDeviceModule outputDevices];
  NSLog(@"[FlutterWebRTC] selectAudioOutput requested deviceId=%@ label=%@ available=%lu playing=%d force=%d",
        deviceId, label, (unsigned long)outputDevices.count, audioDeviceModule.playing, forceTrySet);
  RTCIODevice* matched = nil;
  // Primary match: deviceId. На macOS обычно НЕ работает (см. коммент
  // в selectAudioInput выше). Оставлено для совместимости.
  for (RTCIODevice* device in outputDevices) {
    NSLog(@"[FlutterWebRTC]   out candidate id=%@ name=%@", device.deviceId, device.name);
    if ([deviceId isEqualToString:device.deviceId]) {
      matched = device;
      break;
    }
  }
  // Fallback: точное совпадение по name == label.
  if (matched == nil && label.length > 0) {
    for (RTCIODevice* device in outputDevices) {
      if ([label isEqualToString:device.name]) {
        matched = device;
        NSLog(@"[FlutterWebRTC]   out matched by label → id=%@ name=%@", device.deviceId, device.name);
        break;
      }
    }
  }
  if (matched != nil) {
    // Если ADM в этот момент уже проигрывает (active call) —
    // обычно НЕЛЬЗЯ trySetOutputDevice: он делает stop→set→start
    // audio_unit'а, и во время этого окна (~secs) sip_ua keepalive
    // OPTIONS не получают ответа, WS падает, разговор обрывается.
    // Использовать lazy property setter — применится к следующему init
    // audio_unit (= к следующему звонку).
    //
    // Исключение: `forceTrySet=YES` приходит из Dart-retry'я СРАЗУ после
    // getUserMedia на самом первом звонке. ADM только-только запустил
    // playout на дефолтном устройстве (наш pending ждал, чтобы ADM
    // populate'ился), разговор ещё толком не начался — короткое окно
    // tear-down безопасно, связь успеет восстановиться. И только так
    // мы реально переключим audio_unit с дефолта на выбранное устройство
    // на первом звонке.
    //
    // Если ADM idle (`playing=0`, между звонками) — trySet безопасен
    // всегда.
    if (audioDeviceModule.playing && !forceTrySet) {
      audioDeviceModule.outputDevice = matched;
      NSLog(@"[FlutterWebRTC] setOutputDevice (lazy, playing=1) → %@", matched.name);
    } else if (audioDeviceModule.playing && forceTrySet) {
      // Hot-swap во время активного звонка / loopback warm-up.
      //
      // ВАЖНО: в LiveKit-форке WebRTC-SDK (audioDeviceModuleType:0,
      // CoreAudio ADM) `trySetOutputDevice` возвращает YES, но НЕ
      // перезапускает audio_unit когда ADM уже `playing=1` — он только
      // меняет property, и физический playout продолжается через старое
      // устройство. Поэтому явно крутим цикл stop → set → init → start:
      // audio_unit пересоздаётся с новым CoreAudio device id.
      //
      // ВАЖНО: между stopPlayout и startPlayout НУЖЕН initPlayout, иначе
      // start возвращает -1 (playout не запускается) — libwebrtc требует
      // init после stop для повторной инициализации audio_unit. Без этого
      // юзер теряет голос И микрофон после переключения устройства.
      //
      // Стоит ~50–150 мс глитча в разговоре, но это единственный
      // способ переключить устройство во время звонка с CoreAudio ADM.
      NSInteger stopRc = [audioDeviceModule stopPlayout];
      audioDeviceModule.outputDevice = matched;
      NSInteger initRc = [audioDeviceModule initPlayout];
      NSInteger startRc = [audioDeviceModule startPlayout];
      NSLog(@"[FlutterWebRTC] hot-swap output (force=1, playing=1): stop=%ld → set=%@ → init=%ld → start=%ld",
            (long)stopRc, matched.name, (long)initRc, (long)startRc);
    } else {
      BOOL ok = [audioDeviceModule trySetOutputDevice:matched];
      NSLog(@"[FlutterWebRTC] trySetOutputDevice → %d (device=%@, playing=%d, force=%d)",
            ok, matched.name, audioDeviceModule.playing, forceTrySet);
    }
    self.pendingOutputLabel = nil;
    result(nil);
    return;
  }
  // Match не нашёлся: ADM ещё пустой (раннее apply на старте, до
  // первого peer connection) или device пропал. Сохраняем label как
  // pending — применим в `audioDeviceModuleDidUpdateDevices:`.
  if (label.length > 0) {
    self.pendingOutputLabel = label;
    NSLog(@"[FlutterWebRTC] selectAudioOutput: stored pending label=%@", label);
    result(nil);
    return;
  }
#endif
#if TARGET_OS_IPHONE
  RTCAudioSession* session = [RTCAudioSession sharedInstance];
  NSError* setCategoryError = nil;

  if ([deviceId isEqualToString:@"Speaker"]) {
    [session.session overrideOutputAudioPort:kAudioSessionOverrideAudioRoute_Speaker
                                       error:&setCategoryError];
  } else {
    [session.session overrideOutputAudioPort:kAudioSessionOverrideAudioRoute_None
                                       error:&setCategoryError];
  }

  if (setCategoryError == nil) {
    result(nil);
    return;
  }

  result([FlutterError
      errorWithCode:@"selectAudioOutputFailed"
            message:[NSString
                        stringWithFormat:@"Error: %@", [setCategoryError localizedFailureReason]]
            details:nil]);

#endif
  result([FlutterError errorWithCode:@"selectAudioOutputFailed"
                             message:[NSString stringWithFormat:@"Error: deviceId not found!"]
                             details:nil]);
}

- (void)mediaStreamTrackRelease:(RTCMediaStream*)mediaStream track:(RTCMediaStreamTrack*)track {
  // what's different to mediaStreamTrackStop? only call mediaStream explicitly?
  if (mediaStream && track) {
    track.isEnabled = NO;
    // FIXME this is called when track is removed from the MediaStream,
    // but it doesn't mean it can not be added back using MediaStream.addTrack
    // TODO: [self.localTracks removeObjectForKey:trackID];
    if ([track.kind isEqualToString:@"audio"]) {
      [mediaStream removeAudioTrack:(RTCAudioTrack*)track];
    } else if ([track.kind isEqualToString:@"video"]) {
      [mediaStream removeVideoTrack:(RTCVideoTrack*)track];
    }
  }
}

- (void)mediaStreamTrackHasTorch:(RTCMediaStreamTrack*)track result:(FlutterResult)result {
  if (!self.videoCapturer) {
    result(@NO);
    return;
  }
  if (self.videoCapturer.captureSession.inputs.count == 0) {
    result(@NO);
    return;
  }

  AVCaptureDeviceInput* deviceInput = [self.videoCapturer.captureSession.inputs objectAtIndex:0];
  AVCaptureDevice* device = deviceInput.device;

  result(@([device isTorchModeSupported:AVCaptureTorchModeOn]));
}

- (void)mediaStreamTrackSetTorch:(RTCMediaStreamTrack*)track
                           torch:(BOOL)torch
                          result:(FlutterResult)result {
  if (!self.videoCapturer) {
    NSLog(@"Video capturer is null. Can't set torch");
    return;
  }
  if (self.videoCapturer.captureSession.inputs.count == 0) {
    NSLog(@"Video capturer is missing an input. Can't set torch");
    return;
  }

  AVCaptureDeviceInput* deviceInput = [self.videoCapturer.captureSession.inputs objectAtIndex:0];
  AVCaptureDevice* device = deviceInput.device;

  if (![device isTorchModeSupported:AVCaptureTorchModeOn]) {
    NSLog(@"Current capture device does not support torch. Can't set torch");
    return;
  }

  NSError* error;
  if ([device lockForConfiguration:&error] == NO) {
    NSLog(@"Failed to aquire configuration lock. %@", error.localizedDescription);
    return;
  }

  device.torchMode = torch ? AVCaptureTorchModeOn : AVCaptureTorchModeOff;
  [device unlockForConfiguration];

  result(nil);
}

- (void)mediaStreamTrackSetZoom:(RTCMediaStreamTrack*)track
                           zoomLevel:(double)zoomLevel
                          result:(FlutterResult)result {
#if TARGET_OS_OSX
  NSLog(@"Not supported on macOS. Can't set zoom");
  return;
#endif
#if TARGET_OS_IPHONE
  if (!self.videoCapturer) {
    NSLog(@"Video capturer is null. Can't set zoom");
    return;
  }
  if (self.videoCapturer.captureSession.inputs.count == 0) {
    NSLog(@"Video capturer is missing an input. Can't set zoom");
    return;
  }

  AVCaptureDeviceInput* deviceInput = [self.videoCapturer.captureSession.inputs objectAtIndex:0];
  AVCaptureDevice* device = deviceInput.device;

  NSError* error;
  if ([device lockForConfiguration:&error] == NO) {
    NSLog(@"Failed to acquire configuration lock. %@", error.localizedDescription);
    return;
  }
  
  CGFloat desiredZoomFactor = (CGFloat)zoomLevel;
  device.videoZoomFactor = MAX(1.0, MIN(desiredZoomFactor, device.activeFormat.videoMaxZoomFactor));
  [device unlockForConfiguration];

  result(nil);
#endif
}

- (void)mediaStreamTrackCaptureFrame:(RTCVideoTrack*)track
                              toPath:(NSString*)path
                              result:(FlutterResult)result {
  self.frameCapturer = [[FlutterRTCFrameCapturer alloc] initWithTrack:track
                                                               toPath:path
                                                               result:result];
}

- (void)mediaStreamTrackStop:(RTCMediaStreamTrack*)track {
  if (track) {
    track.isEnabled = NO;
    [self.localTracks removeObjectForKey:track.trackId];
  }
}

- (AVCaptureDevice*)findDeviceForPosition:(AVCaptureDevicePosition)position {
  if (position == AVCaptureDevicePositionUnspecified) {
    return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  }
  NSArray<AVCaptureDevice*>* captureDevices = [RTCCameraVideoCapturer captureDevices];
  for (AVCaptureDevice* device in captureDevices) {
    if (device.position == position) {
      return device;
    }
  }
  if(captureDevices.count > 0) {
    return captureDevices[0];
  }
  return nil;
}

- (AVCaptureDeviceFormat*)selectFormatForDevice:(AVCaptureDevice*)device
                                    targetWidth:(NSInteger)targetWidth
                                   targetHeight:(NSInteger)targetHeight {
  NSArray<AVCaptureDeviceFormat*>* formats =
      [RTCCameraVideoCapturer supportedFormatsForDevice:device];
  AVCaptureDeviceFormat* selectedFormat = nil;
  long currentDiff = INT_MAX;
  for (AVCaptureDeviceFormat* format in formats) {
    CMVideoDimensions dimension = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(format.formatDescription);
#if TARGET_OS_IPHONE
    if (@available(iOS 13.0, *)) {
      if(format.isMultiCamSupported != AVCaptureMultiCamSession.multiCamSupported) {
        continue;
      }
    }
#endif
    //NSLog(@"AVCaptureDeviceFormats,fps %d, dimension: %dx%d", format.videoSupportedFrameRateRanges, dimension.width, dimension.height);
    long diff = labs(targetWidth - dimension.width) + labs(targetHeight - dimension.height);
    if (diff < currentDiff) {
      selectedFormat = format;
      currentDiff = diff;
    } else if (diff == currentDiff &&
               pixelFormat == [self.videoCapturer preferredOutputPixelFormat]) {
      selectedFormat = format;
    }
  }
  return selectedFormat;
}

- (NSInteger)selectFpsForFormat:(AVCaptureDeviceFormat*)format targetFps:(NSInteger)targetFps {
  Float64 maxSupportedFramerate = 0;
  for (AVFrameRateRange* fpsRange in format.videoSupportedFrameRateRanges) {
    maxSupportedFramerate = fmax(maxSupportedFramerate, fpsRange.maxFrameRate);
  }
  return fmin(maxSupportedFramerate, targetFps);
}

@end
