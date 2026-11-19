// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <CoreHaptics/CoreHaptics.h>
#include <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

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
struct Motor::State
{
  using Clock = std::chrono::steady_clock;

  explicit State(CHHapticEngine* engine) : haptic_engine(engine) {}

  bool StartEngineLocked();

  void InvalidatePlayer()
  {
    haptic_player = nil;
    controller_on.reset();
  }

  // PrimeHack for iPad: the CoreHaptics handlers only raise this flag, without the lock, because
  // the engine calls made under the lock could wait on the queue that delivers them. SetState
  // consumes it under the lock.
  void ConsumeEngineLostLocked()
  {
    if (engine_lost.exchange(false))
      InvalidatePlayer();
  }

  std::mutex lock;
  std::atomic<bool> engine_lost{false};
  CHHapticEngine* const haptic_engine;
  id<CHHapticAdvancedPatternPlayer> haptic_player = nil;
  bool recreate_player = false;
  std::optional<bool> controller_on;
  bool device_on = false;
  Clock::time_point next_start_attempt = Clock::time_point::min();
};

Motor::Motor(CHHapticEngine* engine, const std::string name)
    : m_state(std::make_shared<State>(engine)), m_name(std::move(name))
{
  // PrimeHack for iPad: an in-flight callback can outlive the Motor during disconnect.
  const std::weak_ptr<State> weak_state = m_state;
  engine.resetHandler = ^{
    if (const auto state = weak_state.lock())
      state->engine_lost.store(true);
  };

  engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
    if (reason != CHHapticEngineStoppedReasonAudioSessionInterrupt &&
        reason != CHHapticEngineStoppedReasonApplicationSuspended &&
        reason != CHHapticEngineStoppedReasonSystemError)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "Motor received unexpected stopped reason: {}", (NSInteger)reason);
    }
    if (const auto state = weak_state.lock())
      state->engine_lost.store(true);
  };

  // PrimeHack for iPad: configure haptics-only playback before the first start.
  engine.playsHapticsOnly = YES;
  std::lock_guard<std::mutex> guard(m_state->lock);
  if (m_state->StartEngineLocked())
  {
    // PrimeHack for iPad: a successful initial start leaves the first recovery attempt available.
    m_state->next_start_attempt = State::Clock::time_point::min();
  }
}

Motor::~Motor()
{
#if !TARGET_OS_TV
  // PrimeHack for iPad: RemoveDevice destroys a disconnected controller without calling SetState(0),
  // so stop on-device haptics if this motor left them running.
  {
    std::lock_guard<std::mutex> guard(m_state->lock);
    if (m_state->device_on)
    {
      [[DOLDeviceHaptics shared] setState:false];
      m_state->device_on = false;
    }
  }
#endif
  // PrimeHack for iPad: an in-flight handler holds the state alive through its weak_ptr and only
  // sets a flag, so teardown never waits on it.
  CHHapticEngine* engine = m_state->haptic_engine;
  engine.resetHandler = ^{
  };
  engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
  };
  [engine stopWithCompletionHandler:nil];
}

bool Motor::StartEngine()
{
  std::lock_guard<std::mutex> guard(m_state->lock);
  return m_state->StartEngineLocked();
}

bool Motor::State::StartEngineLocked()
{
  if (haptic_player && !recreate_player)
    return true;

  // PrimeHack for iPad: share one monotonic cooldown across startup and lazy restarts.
  const auto now = Clock::now();
  if (now < next_start_attempt)
    return false;
  next_start_attempt = now + std::chrono::seconds(1);

  NSError* error = nil;
  if (![haptic_engine startAndReturnError:&error])
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

  if (!pattern)
  {
    MOTOR_ERROR_LOG("Motor failed to create CHHapticPattern: {}", error);

    return false;
  }
  
  id<CHHapticAdvancedPatternPlayer> player =
      [haptic_engine createAdvancedPlayerWithPattern:pattern error:&error];

  if (!player)
  {
    MOTOR_ERROR_LOG("Motor failed to create CHHapticAdvancedPatternPlayer: {}", error);

    return false;
  }

  [player setLoopEnabled:true];
  [player setLoopEnd:0.0f];

  haptic_player = player;
  recreate_player = false;
  controller_on.reset();

  return true;
}

std::string Motor::GetName() const
{
  return m_name;
}

void Motor::SetState(ControlState state)
{
  std::lock_guard<std::mutex> guard(m_state->lock);
  m_state->ConsumeEngineLostLocked();

  const bool on = state > 0;
  const int destination = RumbleDestination();

#if !TARGET_OS_TV
  // PrimeHack for iPad: route device transitions even while controller recovery is pending.
  const bool device_on = (destination == 0 || destination == 2) && on;
  if (device_on != m_state->device_on)
  {
    [[DOLDeviceHaptics shared] setState:device_on];
    m_state->device_on = device_on;
  }
#endif

  // PrimeHack for iPad: with the destination switched to the device only, the controller still gets
  // its stop if it was rumbling when the destination changed.
  const bool controller_on = on && destination != 0;
  if (controller_on && !m_state->StartEngineLocked())
    return;
  if (!m_state->haptic_player || m_state->controller_on == controller_on)
    return;

  bool result;
  NSError* error = nil;
  if (controller_on)
  {
    result = [m_state->haptic_player startAtTime:CHHapticTimeImmediate error:&error];
  }
  else
  {
    result = [m_state->haptic_player stopAtTime:CHHapticTimeImmediate error:&error];
  }

  if (result)
  {
    m_state->controller_on = controller_on;
    return;
  }

  // PrimeHack for iPad: rebuild the player on the next rumble (at most once a second). A failed stop
  // means the player or engine is gone, so it is silent; record that instead of retrying the stop
  // every input update.
  m_state->recreate_player = true;
  if (controller_on)
    m_state->controller_on.reset();
  else
    m_state->controller_on = false;
  if ([error.domain isEqualToString:CoreHapticsErrorDomain] &&
      error.code == CHHapticErrorCodeInvalidPatternPlayer)
  {
    m_state->InvalidatePlayer();
    if (!controller_on)
      m_state->controller_on = false;
  }
  MOTOR_ERROR_LOG("Motor failed to start/stop haptics: {}", error);
}
} // namespace ciface::iOS
