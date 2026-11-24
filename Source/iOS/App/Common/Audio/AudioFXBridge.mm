// Copyright 2025 DolphiniOS Project
// SPDX-License-Identifier: GPL-2.0-or-later

#import "AudioFXBridge.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#if !TARGET_OS_TV
#import <CoreAudioKit/CoreAudioKit.h>
#endif

// The engine headers are guarded on the core's platform define.
#ifndef IPHONEOS
#define IPHONEOS 1
#endif
#include <mutex>

#include "AudioCommon/AVAudioEngineSoundStream.h"
#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/CoreAudioSoundStream.h"
#include "AudioCommon/IOSAudioInput.h"
#include "AudioCommon/IOSAudioSession.h"
#include "Core/Core.h"
#include "Core/System.h"

static inline AVAudioEngineSound* ActiveEngineLocked()
{
  auto& system = Core::System::GetInstance();
  if (!Core::IsRunning(system))
    return nullptr;
  return dynamic_cast<AVAudioEngineSound*>(system.GetSoundStream());
}

static inline CoreAudioSound* ActiveCoreAudioLocked()
{
  auto& system = Core::System::GetInstance();
  if (!Core::IsRunning(system))
    return nullptr;
  return dynamic_cast<CoreAudioSound*>(system.GetSoundStream());
}

@implementation AudioFXBridge

+ (BOOL)isEngineActive
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  return ActiveEngineLocked() != nullptr;
}

+ (BOOL)isCoreAudioActive
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  return ActiveCoreAudioLocked() != nullptr;
}

+ (NSString*)outputModeName
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  if (AVAudioEngineSound* engine = ActiveEngineLocked())
  {
    switch (engine->GetMode())
    {
    case AVAudioEngineSound::Mode::Stereo:
      return @"stereo";
    case AVAudioEngineSound::Mode::Spatial:
      return @"spatial";
    case AVAudioEngineSound::Mode::Discrete51:
      return @"discrete";
    }
  }
  if (CoreAudioSound* core = ActiveCoreAudioLocked())
    return core->IsSurroundOutput() ? @"discrete" : @"stereo";
  return @"";
}

+ (NSString*)routeDescription
{
  return [NSString
      stringWithUTF8String:AudioCommon::IOSAudioSession::QueryRoute().description.c_str()];
}

+ (NSInteger)routeMaxOutputChannels
{
  return AudioCommon::IOSAudioSession::QueryRoute().max_output_channels;
}

+ (BOOL)routeIsHeadphones
{
  return AudioCommon::IOSAudioSession::QueryRoute().headphones;
}

// MARK: - Plug-in chain

+ (NSArray<NSDictionary*>*)currentEffects
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  AVAudioEngineSound* eng = ActiveEngineLocked();
  if (!eng)
    return @[];
  NSMutableArray* arr = [NSMutableArray array];
  const size_t n = eng->fxCount();
  for (size_t i = 0; i < n; ++i)
  {
    AVAudioUnit* u = eng->fxAt(i);
    if (!u)
      continue;
    [arr addObject:@{
      @"name" : u.name ?: @"Effect",
      @"bypass" : @(u.AUAudioUnit.shouldBypassEffect),
      @"index" : @(i)
    }];
  }
  return arr;
}

+ (NSArray<NSDictionary*>*)availableEffects
{
  NSMutableArray<NSDictionary*>* out = [NSMutableArray array];
  AVAudioUnitComponentManager* mgr = [AVAudioUnitComponentManager sharedAudioUnitComponentManager];
  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Effect;
  for (AVAudioUnitComponent* c in [mgr componentsMatchingDescription:desc])
  {
    const AudioComponentDescription acd = c.audioComponentDescription;
    NSString* ident = [NSString
        stringWithFormat:@"type:%u sub:%u manu:%u", (unsigned)acd.componentType,
                         (unsigned)acd.componentSubType, (unsigned)acd.componentManufacturer];
    [out addObject:@{@"name" : c.name ?: @"Effect", @"identifier" : ident}];
  }
  return out;
}

+ (BOOL)addEffectWithName:(NSString*)name
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  AVAudioEngineSound* eng = ActiveEngineLocked();
  if (!eng)
    return NO;
  return eng->addEffectWithIdentifier(name.UTF8String) ? YES : NO;
}

+ (void)removeEffectAt:(NSUInteger)index
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  if (AVAudioEngineSound* eng = ActiveEngineLocked())
    eng->removeEffectAt(index);
}

+ (void)moveEffectFrom:(NSUInteger)from to:(NSUInteger)to
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  if (AVAudioEngineSound* eng = ActiveEngineLocked())
    eng->moveEffect(from, to);
}

+ (void)setEffectAt:(NSUInteger)index bypassed:(BOOL)bypassed
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  if (AVAudioEngineSound* eng = ActiveEngineLocked())
    eng->setEffectBypass(index, bypassed);
}

+ (void)saveEffectChain
{
  std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
  if (AVAudioEngineSound* eng = ActiveEngineLocked())
    eng->saveEffectChain();
}

+ (void)requestEffectViewControllerAt:(NSUInteger)index
                           completion:(void (^)(UIViewController* _Nullable vc))completion
{
  AVAudioUnit* u = nil;
  {
    std::lock_guard<std::recursive_mutex> lock(AudioCommon::GetSoundStreamMutex());
    AVAudioEngineSound* eng = ActiveEngineLocked();
    u = eng ? eng->fxAt(index) : nil;
  }
  if (!u)
  {
    completion(nil);
    return;
  }
#if !TARGET_OS_TV
  [u.AUAudioUnit requestViewControllerWithCompletionHandler:^(UIViewController* _Nullable vc) {
    completion(vc);
  }];
#else
  completion(nil);
#endif
}

// MARK: - Microphone

+ (BOOL)microphoneSupported
{
  return IOSAudioInput::IsSupported();
}

+ (NSInteger)microphonePermission
{
  switch (IOSAudioInput::GetPermission())
  {
  case IOSAudioInput::Permission::Undetermined:
    return 0;
  case IOSAudioInput::Permission::Denied:
    return 1;
  case IOSAudioInput::Permission::Granted:
    return 2;
  }
  return 0;
}

+ (void)requestMicrophonePermission:(void (^)(BOOL granted))completion
{
  IOSAudioInput::RequestPermission([completion](bool granted) {
    dispatch_async(dispatch_get_main_queue(), ^{
      completion(granted);
    });
  });
}

@end
