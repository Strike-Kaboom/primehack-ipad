// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <CoreHaptics/CoreHaptics.h>
#include <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include "Common/Logging/Log.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/iOS/Motor.h"

#define MOTOR_ERROR_LOG(x, y) ERROR_LOG_FMT(CONTROLLERINTERFACE, x, [[y localizedDescription] UTF8String])

namespace
{
/// User-selected rumble destination (NSUserDefaults "rumble_destination"):
/// 0 = device haptics, 1 = controller rumble (default), 2 = both. tvOS has no
/// device to hold, so it always uses controller rumble.
int RumbleDestination()
{
#if TARGET_OS_TV
  return 1;
#else
  NSNumber* value = [[NSUserDefaults standardUserDefaults] objectForKey:@"rumble_destination"];
  return value ? value.intValue : 1;
#endif
}
}  // namespace

#if !TARGET_OS_TV
/// Process-wide on-device (Taptic Engine) haptics player, mirroring the
/// per-controller Motor engine but bound to the phone instead of a controller.
/// Shared because every emulated motor routes through the same physical device.
@interface DOLDeviceHaptics : NSObject
+ (instancetype)shared;
- (void)setState:(bool)on;
@end

@implementation DOLDeviceHaptics {
  CHHapticEngine* _engine;
  id<CHHapticAdvancedPatternPlayer> _player;
  bool _started;
  bool _on;
}

+ (instancetype)shared
{
  static DOLDeviceHaptics* shared;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    shared = [DOLDeviceHaptics new];
  });
  return shared;
}

- (instancetype)init
{
  if ((self = [super init]))
  {
    [self prepare];
  }
  return self;
}

- (void)prepare
{
  if (!CHHapticEngine.capabilitiesForHardware.supportsHaptics)
    return;

  NSError* error = nil;
  _engine = [[CHHapticEngine alloc] initAndReturnError:&error];
  if (!_engine)
    return;

  __weak DOLDeviceHaptics* weak_self = self;
  _engine.resetHandler = ^{
    DOLDeviceHaptics* strong_self = weak_self;
    if (strong_self)
    {
      strong_self->_started = false;
      strong_self->_player = nil;
      [strong_self prepare];
    }
  };
  _engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
    DOLDeviceHaptics* strong_self = weak_self;
    if (strong_self)
      strong_self->_started = false;
  };

  CHHapticEventParameter* intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
  CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                       parameters:@[intensity]
                                                     relativeTime:0.0f
                                                         duration:1.0f];
  CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
  if (!pattern)
    return;

  _player = [_engine createAdvancedPlayerWithPattern:pattern error:&error];
  [_player setLoopEnabled:true];
  [_player setLoopEnd:0.0f];
}

- (void)setState:(bool)on
{
  @synchronized(self)
  {
    if (!_player || on == _on)
      return;

    _on = on;

    NSError* error = nil;
    if (!_started)
    {
      if (![_engine startAndReturnError:&error])
        return;
      _started = true;
    }

    if (on)
      [_player startAtTime:CHHapticTimeImmediate error:&error];
    else
      [_player stopAtTime:CHHapticTimeImmediate error:&error];
  }
}
@end
#endif

namespace ciface::iOS
{
Motor::Motor(CHHapticEngine* engine, const std::string name) : m_haptic_engine(engine), m_name(std::move(name))
{
  std::lock_guard<std::mutex> guard(m_lock);

  // PrimeHack for iPad: install the handlers before the first start, and recover from any stop.
  // Before, a first start that failed (the pad is enumerated while the app is still launching) or
  // a stop for an unlisted reason left the motor dead for the rest of the session: no rumble.
  m_haptic_engine.resetHandler = ^{
    std::lock_guard<std::mutex> reset_guard(m_lock);

    m_player_created = false;

    m_haptic_player = nil;

    StartEngine();
  };

  m_haptic_engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
    std::lock_guard<std::mutex> stopped_guard(m_lock);

    if (reason != CHHapticEngineStoppedReasonAudioSessionInterrupt &&
        reason != CHHapticEngineStoppedReasonApplicationSuspended &&
        reason != CHHapticEngineStoppedReasonSystemError)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "Motor received unexpected stopped reason: {}", (NSInteger)reason);
    }
    // Restarted on the next rumble request (SetState), rate-limited.
    m_player_needs_restart = true;
  };

  // Haptics only: no audio resources, so an audio-session interruption can't stop the rumble.
  m_haptic_engine.playsHapticsOnly = YES;

  StartEngine();
}

Motor::~Motor()
{
  std::lock_guard<std::mutex> guard(m_lock);

  // The blocks capture `this`; make sure a late callback can't reach a destroyed Motor.
  m_haptic_engine.resetHandler = ^{
  };
  m_haptic_engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
  };

  if (m_player_created)
  {
    [m_haptic_engine stopWithCompletionHandler:nil];
  }
}

bool Motor::StartEngine()
{
  NSError* error;
  
  if (![m_haptic_engine startAndReturnError:&error])
  {
    MOTOR_ERROR_LOG("Motor failed to start CHHapticEngine: {}", error);

    return false;
  }
  
  CHHapticEventParameter* intensity_param = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
  // PrimeHack for iPad: low sharpness. The default (0.5) plays as a faint high buzz on the
  // DualSense's voice-coil actuators; a low value gives a deep rumble you can feel in the grips.
  CHHapticEventParameter* sharpness_param = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.1f];

  CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                       parameters:@[intensity_param, sharpness_param]
                                                     relativeTime:0.0f
                                                         duration:1.0f];

  error = nil;
  CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];

  if (error != nil)
  {
    MOTOR_ERROR_LOG("Motor failed to create CHHapticPattern: {}", error);

    return false;
  }
  
  m_haptic_player = [m_haptic_engine createAdvancedPlayerWithPattern:pattern error:&error];

  if (error != nil)
  {
    MOTOR_ERROR_LOG("Motor failed to create CHHapticAdvancedPatternPlayer: {}", error);

    return false;
  }

  [m_haptic_player setLoopEnabled:true];
  [m_haptic_player setLoopEnd:0.0f];

  m_player_created = true;
  m_player_needs_restart = false;

  return true;
}

std::string Motor::GetName() const
{
  return m_name;
}

void Motor::SetState(ControlState state)
{
  std::lock_guard<std::mutex> guard(m_lock);

  if (state == m_last_state)
  {
    return;
  }

  m_last_state = state;

  const int destination = RumbleDestination();

#if !TARGET_OS_TV
  if (destination == 0 || destination == 2)
  {
    [[DOLDeviceHaptics shared] setState:(state > 0)];
  }

  // Device-only: leave the controller motor untouched.
  if (destination == 0)
  {
    return;
  }
#endif

  // PrimeHack for iPad: (re)start the engine when rumble is wanted and it isn't running, at most once
  // a second. On failure, forget the state so the next rumble request tries again.
  if (!m_player_created || m_player_needs_restart)
  {
    if (state <= 0)
      return;
    const double now = CFAbsoluteTimeGetCurrent();
    if (now < m_next_start_attempt)
    {
      m_last_state = 0.0;
      return;
    }
    if (!StartEngine())
    {
      m_next_start_attempt = now + 1.0;
      m_last_state = 0.0;
      return;
    }
  }

  bool result;
  NSError* error;
  
  if (state > 0)
  {
    result = [m_haptic_player startAtTime:CHHapticTimeImmediate error:&error];
  }
  else
  {
    result = [m_haptic_player stopAtTime:CHHapticTimeImmediate error:&error];
  }

  if (!result)
  {
    MOTOR_ERROR_LOG("Motor failed to start/stop haptics: {}", error);
  }
}
} // namespace ciface::iOS
