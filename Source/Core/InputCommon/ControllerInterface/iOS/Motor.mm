// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <CoreHaptics/CoreHaptics.h>
#include <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

#include "Common/Logging/Log.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/iOS/Motor.h"

#define MOTOR_ERROR_LOG(x, y) ERROR_LOG_FMT(CONTROLLERINTERFACE, x, [[y localizedDescription] UTF8String])

namespace
{
dispatch_queue_t HapticsQueue()
{
  static dispatch_queue_t queue = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    dispatch_queue_attr_t attr =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    queue = dispatch_queue_create("org.dolphin-emu.ios.haptics", attr);
  });
  return queue;
}

std::atomic<float> s_rumble_strength{1.0f};
#if !TARGET_OS_TV
std::atomic<int> s_rumble_destination{1};
#endif

void RefreshRumbleSettings()
{
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
#if !TARGET_OS_TV
  NSNumber* dest = [defaults objectForKey:@"rumble_destination"];
  s_rumble_destination.store(dest ? dest.intValue : 1, std::memory_order_relaxed);
#endif
  NSNumber* pct = [defaults objectForKey:@"primehack_rumble_strength"];
  const int clamped_pct = pct ? std::clamp(pct.intValue, 25, 100) : 100;
  s_rumble_strength.store(static_cast<float>(clamped_pct) / 100.0f, std::memory_order_relaxed);
}

void EnsureRumbleDefaultsObserver()
{
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    RefreshRumbleSettings();
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSUserDefaultsDidChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  RefreshRumbleSettings();
                }];
  });
}

/// User-selected rumble destination (NSUserDefaults "rumble_destination"):
/// 0 = device haptics, 1 = controller rumble (default), 2 = both. tvOS has no
/// device to hold, so it always uses controller rumble.
int RumbleDestination()
{
#if TARGET_OS_TV
  return 1;
#else
  EnsureRumbleDefaultsObserver();
  return s_rumble_destination.load(std::memory_order_relaxed);
#endif
}

float RumbleStrength()
{
  EnsureRumbleDefaultsObserver();
  return s_rumble_strength.load(std::memory_order_relaxed);
}
}  // namespace

#if !TARGET_OS_TV
/// Process-wide on-device (Taptic Engine) haptics player, mirroring the
/// per-controller Motor engine but bound to the phone instead of a controller.
/// Shared because every emulated motor routes through the same physical device.
@interface DOLDeviceHaptics : NSObject
+ (instancetype)shared;
- (void)setState:(bool)on strength:(float)strength;
@end

@implementation DOLDeviceHaptics {
  CHHapticEngine* _engine;
  id<CHHapticAdvancedPatternPlayer> _player;
  bool _started;
  bool _on;
  float _appliedStrength;
  // CoreHaptics callbacks run on an internal queue; set an atomic flag there instead of mutating
  // ARC ivars or taking @synchronized(self) (which can deadlock against startAndReturnError:).
  std::atomic<bool> _engineLost;
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
    _engineLost.store(false, std::memory_order_relaxed);
    _appliedStrength = -1.0f;
    [self prepareLocked];
  }
  return self;
}

- (void)prepareLocked
{
  if (!CHHapticEngine.capabilitiesForHardware.supportsHaptics)
    return;

  _started = false;
  _on = false;
  _appliedStrength = -1.0f;
  _player = nil;
  _engine = nil;

  NSError* error = nil;
  _engine = [[CHHapticEngine alloc] initAndReturnError:&error];
  if (!_engine)
    return;

  _engine.playsHapticsOnly = YES;

  __weak DOLDeviceHaptics* weak_self = self;
  _engine.resetHandler = ^{
    DOLDeviceHaptics* strong_self = weak_self;
    if (strong_self)
      strong_self->_engineLost.store(true, std::memory_order_release);
  };
  _engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
    DOLDeviceHaptics* strong_self = weak_self;
    if (strong_self)
      strong_self->_engineLost.store(true, std::memory_order_release);
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

- (void)setState:(bool)on strength:(float)strength
{
  @synchronized(self)
  {
    if (_engineLost.exchange(false, std::memory_order_acq_rel))
    {
      _started = false;
      _on = false;
      _appliedStrength = -1.0f;
      _player = nil;
      if (on)
        [self prepareLocked];
    }

    if (!on)
    {
      if (!_on || !_player || !_started)
      {
        _on = false;
        return;
      }
      NSError* error = nil;
      [_player stopAtTime:CHHapticTimeImmediate error:&error];
      _on = false;
      return;
    }

    if (!_player)
      [self prepareLocked];
    if (!_player)
      return;

    NSError* error = nil;
    if (!_started)
    {
      if (![_engine startAndReturnError:&error])
        return;
      _started = true;
    }

    if (std::fabs(_appliedStrength - strength) > 0.01f)
    {
      CHHapticDynamicParameter* param = [[CHHapticDynamicParameter alloc]
          initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                        value:strength
                 relativeTime:0.0f];
      if ([_player sendParameters:@[ param ] atTime:CHHapticTimeImmediate error:&error])
        _appliedStrength = strength;
    }

    if (_on)
      return;

    if ([_player startAtTime:CHHapticTimeImmediate error:&error])
      _on = true;
    else
      _started = false;
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

  // Must only be called on HapticsQueue(). Never holds `lock` across CoreHaptics calls.
  bool StartEngineOnQueue();
  void ApplyOnQueue();

  void InvalidatePlayerOnQueue()
  {
    haptic_player = nil;
    applied_controller_on.reset();
    applied_strength = -1.0f;
  }

  void ConsumeEngineLostOnQueue()
  {
    if (engine_lost.exchange(false, std::memory_order_acq_rel))
      InvalidatePlayerOnQueue();
  }

  // Short-held mutex guarding only the CPU->HapticsQueue handoff fields below.
  std::mutex lock;
  bool desired_controller_on = false;
  bool desired_device_on = false;
  float desired_strength = 1.0f;
  bool update_scheduled = false;
  bool destroyed = false;

  // Lock-free flags: `engine_lost` is written by CoreHaptics callbacks; `needs_retry` is written
  // by HapticsQueue() so SetState can re-trigger recovery after a cooldown expires.
  std::atomic<bool> engine_lost{false};
  std::atomic<bool> needs_retry{false};

  // Owned exclusively by HapticsQueue() (no mutex needed when accessed on that serial queue).
  CHHapticEngine* const haptic_engine;
  id<CHHapticAdvancedPatternPlayer> haptic_player = nil;
  bool recreate_player = false;
  std::optional<bool> applied_controller_on;
  bool applied_device_on = false;
  float applied_device_strength = -1.0f;
  float applied_strength = -1.0f;
  Clock::time_point next_start_attempt = Clock::time_point::min();
};

Motor::Motor(CHHapticEngine* engine, const std::string name)
    : m_state(std::make_shared<State>(engine)), m_name(std::move(name))
{
  // PrimeHack for iPad: an in-flight callback can outlive the Motor during disconnect.
  const std::weak_ptr<State> weak_state = m_state;
  engine.resetHandler = ^{
    if (const auto state = weak_state.lock())
      state->engine_lost.store(true, std::memory_order_release);
  };

  engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
    if (reason != CHHapticEngineStoppedReasonAudioSessionInterrupt &&
        reason != CHHapticEngineStoppedReasonApplicationSuspended &&
        reason != CHHapticEngineStoppedReasonSystemError)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "Motor received unexpected stopped reason: {}",
                    (NSInteger)reason);
    }
    if (const auto state = weak_state.lock())
      state->engine_lost.store(true, std::memory_order_release);
  };

  // PrimeHack for iPad: configure haptics-only playback and start asynchronously on HapticsQueue
  // so Bluetooth controller connect/reconnect never stalls the main or CPU thread.
  engine.playsHapticsOnly = YES;
  dispatch_async(HapticsQueue(), ^{
    @autoreleasepool
    {
      const auto state = weak_state.lock();
      if (!state)
        return;
      {
        std::lock_guard<std::mutex> guard(state->lock);
        if (state->destroyed)
          return;
      }
      if (state->StartEngineOnQueue())
      {
        // A successful initial start leaves the first recovery attempt immediately available.
        state->next_start_attempt = State::Clock::time_point::min();
      }
    }
  });
}

Motor::~Motor()
{
  {
    std::lock_guard<std::mutex> guard(m_state->lock);
    m_state->destroyed = true;
  }

  // Stop further CoreHaptics callbacks immediately.
  CHHapticEngine* engine = m_state->haptic_engine;
  engine.resetHandler = ^{
  };
  engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
  };

  // Serialize teardown on HapticsQueue so it runs strictly after any in-flight start/stop call,
  // and turns off on-device haptics if this motor left them running.
  std::shared_ptr<State> state = m_state;
  dispatch_async(HapticsQueue(), ^{
    @autoreleasepool
    {
#if !TARGET_OS_TV
      if (state->applied_device_on)
      {
        [[DOLDeviceHaptics shared] setState:false strength:1.0f];
        state->applied_device_on = false;
      }
#endif
      state->InvalidatePlayerOnQueue();
      [state->haptic_engine stopWithCompletionHandler:nil];
    }
  });
}

bool Motor::StartEngine()
{
  const std::weak_ptr<State> weak_state = m_state;
  dispatch_async(HapticsQueue(), ^{
    @autoreleasepool
    {
      const auto state = weak_state.lock();
      if (!state)
        return;
      {
        std::lock_guard<std::mutex> guard(state->lock);
        if (state->destroyed)
          return;
      }
      state->StartEngineOnQueue();
    }
  });
  return true;
}

bool Motor::State::StartEngineOnQueue()
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

  CHHapticEventParameter* intensity_param =
      [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                    value:1.0f];
  // PrimeHack for iPad: low sharpness. The default (0.5) plays as a faint high buzz on the
  // DualSense's voice-coil actuators; a low value gives a deep rumble you can feel in the grips.
  CHHapticEventParameter* sharpness_param =
      [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                    value:0.1f];

  CHHapticEvent* event =
      [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                    parameters:@[intensity_param, sharpness_param]
                                  relativeTime:0.0f
                                      duration:1.0f];

  error = nil;
  CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event]
                                                          parameters:@[]
                                                               error:&error];

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
  applied_controller_on.reset();
  applied_strength = -1.0f;

  return true;
}

void Motor::State::ApplyOnQueue()
{
  for (int pass = 0; pass < 2; ++pass)
  {
    bool want_controller = false;
    bool want_device = false;
    float want_strength = 1.0f;
    {
      std::lock_guard<std::mutex> guard(lock);
      if (pass == 0)
        update_scheduled = false;
      if (destroyed)
        return;
      want_controller = desired_controller_on;
      want_device = desired_device_on;
      want_strength = desired_strength;
    }

#if !TARGET_OS_TV
    // Route on-device Taptic Engine transitions off the CPU thread as well.
    if (want_device != applied_device_on ||
        (want_device && std::fabs(applied_device_strength - want_strength) > 0.01f))
    {
      [[DOLDeviceHaptics shared] setState:want_device strength:want_strength];
      applied_device_on = want_device;
      applied_device_strength = want_strength;
    }
#endif

    ConsumeEngineLostOnQueue();

    if (want_controller)
    {
      if (!StartEngineOnQueue())
      {
        needs_retry.store(true, std::memory_order_release);
        return;
      }
      // If CoreHaptics signalled an interruption mid-start, discard the stale player and retry
      // on the next rumble update rather than playing into a dead engine.
      if (engine_lost.load(std::memory_order_acquire))
      {
        ConsumeEngineLostOnQueue();
        needs_retry.store(true, std::memory_order_release);
        return;
      }
    }
    else
    {
      needs_retry.store(false, std::memory_order_release);
    }

    if (haptic_player && want_controller && std::fabs(applied_strength - want_strength) > 0.01f)
    {
      NSError* param_error = nil;
      CHHapticDynamicParameter* intensity_control = [[CHHapticDynamicParameter alloc]
          initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                        value:want_strength
                 relativeTime:0.0f];
      if ([haptic_player sendParameters:@[ intensity_control ]
                                 atTime:CHHapticTimeImmediate
                                  error:&param_error])
      {
        applied_strength = want_strength;
      }
      else
      {
        recreate_player = true;
        MOTOR_ERROR_LOG("Motor failed to update rumble strength: {}", param_error);
      }
    }

    if (haptic_player && applied_controller_on != want_controller)
    {
      NSError* error = nil;
      const bool result =
          want_controller ? [haptic_player startAtTime:CHHapticTimeImmediate error:&error]
                          : [haptic_player stopAtTime:CHHapticTimeImmediate error:&error];
      if (result)
      {
        applied_controller_on = want_controller;
        needs_retry.store(false, std::memory_order_release);
      }
      else
      {
        recreate_player = true;
        if (want_controller)
        {
          applied_controller_on.reset();
          needs_retry.store(true, std::memory_order_release);
        }
        else
        {
          applied_controller_on = false;
          needs_retry.store(false, std::memory_order_release);
        }
        if ([error.domain isEqualToString:CoreHapticsErrorDomain] &&
            error.code == CHHapticErrorCodeInvalidPatternPlayer)
        {
          InvalidatePlayerOnQueue();
          if (!want_controller)
            applied_controller_on = false;
        }
        MOTOR_ERROR_LOG("Motor failed to start/stop haptics: {}", error);
      }
    }

    // Re-check whether desired state changed while CoreHaptics XPC was in flight (e.g. rumble
    // already ended while the engine was restarting); if unchanged, we are done.
    {
      std::lock_guard<std::mutex> guard(lock);
      if (destroyed ||
          (desired_controller_on == want_controller && desired_device_on == want_device &&
           std::fabs(desired_strength - want_strength) <= 0.01f))
      {
        return;
      }
    }
  }
}

std::string Motor::GetName() const
{
  return m_name;
}

void Motor::SetState(ControlState state)
{
  const bool on = state > 0;
  const int destination = RumbleDestination();
  const float strength = RumbleStrength();
#if !TARGET_OS_TV
  const bool want_device = (destination == 0 || destination == 2) && on;
#else
  const bool want_device = false;
#endif
  // With destination switched to device-only, the controller still gets its stop if it was on.
  const bool want_controller = on && destination != 0;

  bool should_dispatch = false;
  {
    std::lock_guard<std::mutex> guard(m_state->lock);
    if (m_state->destroyed)
      return;

    const bool unchanged =
        (m_state->desired_controller_on == want_controller) &&
        (m_state->desired_device_on == want_device) &&
        (!(want_controller || want_device) ||
         std::fabs(m_state->desired_strength - strength) <= 0.01f) &&
        !m_state->engine_lost.load(std::memory_order_relaxed) &&
        !(want_controller && m_state->needs_retry.load(std::memory_order_relaxed));
    if (unchanged)
      return;

    m_state->desired_controller_on = want_controller;
    m_state->desired_device_on = want_device;
    m_state->desired_strength = strength;
    if (!m_state->update_scheduled)
    {
      m_state->update_scheduled = true;
      should_dispatch = true;
    }
  }

  if (!should_dispatch)
    return;

  const std::weak_ptr<State> weak_state = m_state;
  dispatch_async(HapticsQueue(), ^{
    @autoreleasepool
    {
      if (const auto s = weak_state.lock())
        s->ApplyOnQueue();
    }
  });
}
} // namespace ciface::iOS
