// Copyright 2025 DolphiniOS Project
// SPDX-License-Identifier: GPL-2.0-or-later

#import "AudioSessionManager.h"

#import <AVFoundation/AVFoundation.h>

#ifndef IPHONEOS
#define IPHONEOS 1
#endif
#include "AudioCommon/IOSAudioSession.h"
#import "Core/Config/iOSSettings.h"

#import "Swift.h"

/// 10.7 ms at 48 kHz: half the callbacks of the old 5 ms request for a difference the game's own
/// 80 ms mixer buffer makes inaudible.
static const NSTimeInterval kPreferredIOBufferDuration = 512.0 / 48000.0;
static const double kPreferredSampleRate = 48000.0;

@implementation AudioSessionManager {
  id _routeChangeObserver;
  BOOL _hookRegistered;
  BOOL _microphoneActive;
}

/// "HDMI:Living Room TV, BluetoothA2DP:AirPods Pro" — the output ports the session is actually
/// using. Logged at activation and on every route change so an "I hear it on the TV, not the
/// AirPods" report can be read straight from the console.
+ (NSString*)describeRoute:(AVAudioSessionRouteDescription*)route {
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  for (AVAudioSessionPortDescription* out in route.outputs) {
    [parts addObject:[NSString stringWithFormat:@"%@:%@", out.portType, out.portName]];
  }
  return parts.count > 0 ? [parts componentsJoinedByString:@", "] : @"(none)";
}

- (void)observeRouteChangesOnce {
  if (_routeChangeObserver != nil) {
    return;
  }
  _routeChangeObserver = [[NSNotificationCenter defaultCenter]
      addObserverForName:AVAudioSessionRouteChangeNotification
                  object:[AVAudioSession sharedInstance]
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification* note) {
                NSNumber* reason = note.userInfo[AVAudioSessionRouteChangeReasonKey];
                NSLog(@"[Audio] route changed (reason %@) -> %@", reason,
                      [AudioSessionManager describeRoute:[AVAudioSession sharedInstance].currentRoute]);
                // PrimeHack for iPad: headphones went away (AirPods back in their case or switched to
                // another device, a wired or USB-C set unplugged). Tell the game screen, which pauses
                // rather than carry on through the iPad's speakers in a quiet cabin. Delivered on the
                // main queue (this observer's queue).
                if (reason.unsignedIntegerValue == AVAudioSessionRouteChangeReasonOldDeviceUnavailable) {
                  auto isHeadphonePort = ^BOOL(AVAudioSessionPortDescription* output) {
                    NSString* type = output.portType;
                    return [type isEqualToString:AVAudioSessionPortHeadphones] ||
                           [type isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
                           [type isEqualToString:AVAudioSessionPortBluetoothLE] ||
                           [type isEqualToString:AVAudioSessionPortBluetoothHFP] ||
                           [type isEqualToString:AVAudioSessionPortUSBAudio];
                  };
                  AVAudioSessionRouteDescription* previous =
                      note.userInfo[AVAudioSessionRouteChangePreviousRouteKey];
                  BOOL hadHeadphones = NO;
                  for (AVAudioSessionPortDescription* output in previous.outputs) {
                    if (isHeadphonePort(output)) {
                      hadHeadphones = YES;
                      break;
                    }
                  }
                  BOOL stillHasHeadphones = NO;
                  for (AVAudioSessionPortDescription* output in [AVAudioSession sharedInstance].currentRoute.outputs) {
                    if (isHeadphonePort(output)) {
                      stillHasHeadphones = YES;
                      break;
                    }
                  }
                  if (hadHeadphones && !stillHasHeadphones) {
                    [[NSNotificationCenter defaultCenter]
                        postNotificationName:@"DOLHeadphonesDisconnectedNotification"
                                      object:nil];
                  }
                }
              }];
}

- (void)registerMicrophoneHookOnce
{
  if (_hookRegistered)
  {
    return;
  }
  _hookRegistered = YES;
  __weak AudioSessionManager* weakSelf = self;
  AudioCommon::IOSAudioSession::SetMicrophoneSessionHook([weakSelf](bool active) {
    AudioSessionManager* strongSelf = weakSelf;
    if (!strongSelf)
    {
      return;
    }
    strongSelf->_microphoneActive = active ? YES : NO;
    NSLog(@"[Audio] microphone %@", active ? @"opened" : @"closed");
    [strongSelf setSessionCategory];
  });
}

+ (AudioSessionManager*)shared {
  static AudioSessionManager* sharedInstance = nil;
  static dispatch_once_t onceToken;

  dispatch_once(&onceToken, ^{
    sharedInstance = [[self alloc] init];
  });

  return sharedInstance;
}

- (BOOL)microphoneActive
{
  return _microphoneActive;
}

/// Serial: activation is a blocking IPC to the audio server, so the main thread never waits on it.
+ (dispatch_queue_t)sessionQueue {
  static dispatch_queue_t queue;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    queue = dispatch_queue_create("org.dolphin-emu.ios.audio-session", DISPATCH_QUEUE_SERIAL);
  });
  return queue;
}

- (void)setSessionCategory {
  [self registerMicrophoneHookOnce];
  [self observeRouteChangesOnce];
  // Capture _microphoneActive on main before dispatching to sessionQueue to avoid a data race.
  const BOOL micActive = _microphoneActive;
  // Every caller is on the main thread (launch, the mute-switch setting, and the core's microphone
  // hook, which IOSAudioSession already posts to main asynchronously), and none waits on the result.
  dispatch_async([AudioSessionManager sessionQueue], ^{
    [self applySessionCategoryWithMicActive:micActive];
  });
}

- (void)applySessionCategoryWithMicActive:(BOOL)micActive {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  const BOOL mixWithOthers = Config::Get(Config::MAIN_AUDIO_MIX_WITH_OTHERS);

#if !TARGET_OS_TV
  if (micActive)
  {
    // Recording: Bluetooth stays on A2DP for output (built-in microphone for input) rather than
    // dropping to the low-quality HFP profile, and the speaker stays the default output. tvOS has
    // no app-accessible microphone, so the core never asks for this there.
    AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionAllowBluetoothA2DP |
                                            AVAudioSessionCategoryOptionDefaultToSpeaker |
                                            AVAudioSessionCategoryOptionAllowAirPlay;
    if (mixWithOthers)
    {
      options |= AVAudioSessionCategoryOptionMixWithOthers;
    }
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                         mode:AVAudioSessionModeDefault
                      options:options
                        error:&error])
    {
      NSLog(@"[Audio] setCategory(PlayAndRecord) failed: %@", error);
    }
  }
  else
#endif
  {
    AudioMuteSwitchMode mode = (AudioMuteSwitchMode)Config::Get(Config::MAIN_MUTE_SWITCH_MODE);
    NSString* category = AVAudioSessionCategoryPlayback;
    AVAudioSessionCategoryOptions options =
        mixWithOthers ? AVAudioSessionCategoryOptionMixWithOthers : 0;
#if !TARGET_OS_TV
    if (mode == AudioMuteSwitchModeObey)
    {
      // SoloAmbient forbids MixWithOthers; Ambient obeys the mute switch AND mixes with others.
      category = mixWithOthers ? AVAudioSessionCategoryAmbient : AVAudioSessionCategorySoloAmbient;
      if (mixWithOthers)
        options = 0;
    }
#else
    (void)mode;
#endif
    // Playback categories route to Bluetooth and AirPlay on their own; the AllowBluetoothA2DP /
    // AllowAirPlay options are PlayAndRecord-only and tvOS rejects them. Ambient/SoloAmbient and
    // MixWithOthers require Default mode on iOS; fall back to Default on any failure.
    const bool preferDefaultMode =
        mixWithOthers || ![category isEqualToString:AVAudioSessionCategoryPlayback];
    NSString* sessionMode =
        preferDefaultMode ? AVAudioSessionModeDefault : AVAudioSessionModeMoviePlayback;
    if (![session setCategory:category mode:sessionMode options:options error:&error])
    {
      if (![sessionMode isEqualToString:AVAudioSessionModeDefault])
      {
        error = nil;
        if (![session setCategory:category
                             mode:AVAudioSessionModeDefault
                          options:options
                            error:&error])
        {
          NSLog(@"[Audio] setCategory(%@, Default) failed: %@", category, error);
        }
      }
      else
      {
        NSLog(@"[Audio] setCategory(%@) failed: %@", category, error);
      }
    }
  }

  [session setPreferredSampleRate:kPreferredSampleRate error:nil];
  [session setPreferredIOBufferDuration:kPreferredIOBufferDuration error:nil];
  NSError* activateError = nil;
  if (![session setActive:YES error:&activateError]) {
    NSLog(@"[Audio] setActive failed: %@", activateError);
  }
  NSLog(@"[Audio] session category=%@ mode=%@ options=0x%lx mic=%d route=%@", session.category,
        session.mode, (unsigned long)session.categoryOptions, (int)micActive,
        [AudioSessionManager describeRoute:session.currentRoute]);
}

@end
