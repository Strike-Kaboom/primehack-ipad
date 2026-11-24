#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "Core/PrimeHack/HackManager.h"
#include "Core/PrimeHack/AddressDB.h"
#include "Core/PrimeHack/EmuVariableManager.h"

#include "InputCommon/ControlReference/ControlReference.h"

// Naming scheme will match dolphin as this place acts as an interface between the hack & dolphin proper
namespace prime {
void InitializeHack();

// True for the game IDs PrimeHack drives (its kSupportedTitles: the NTSC-U and PAL releases).
bool IsSupportedGameID(std::string_view game_id);

#ifdef IPHONEOS
// PrimeHack for iPad: during a Metroid Prime game, keeps player 1's PrimeHack controller layout
// (and, on Wii, the Nunchuk) in place when something reloads the controller config mid-game.
void EnforceIOSControllerLayout(Game game);
// Applies iOS radial deadzone, response curve, and horizontal/vertical sensitivity to the camera stick.
std::tuple<double, double> ShapeIOSPrimeStick(double x, double y);
#endif
void EnableDefaultMods();

bool CheckBeamCtl(int beam_num);
bool CheckVisorCtl(int visor_num);
bool CheckBeamScrollCtl(bool direction);
bool CheckVisorScrollCtl(bool direction);
bool CheckSpringBallCtl();
bool ImprovedMotionControls();
bool CheckForward();
bool CheckBack();
bool CheckLeft();
bool CheckRight();
bool CheckJump();

bool CheckGrappleCtl();
bool GrappleTappingMode();
bool GrappleCtlBound();

void SetEFBToTexture(bool toggle);
// PrimeHack for iPad: removes the run-time EFB override, so the user's or game INI's setting applies again.
void ClearEFBOverride();
bool UseMPAutoEFB();
bool LockCameraInPuzzles();
bool GetEFBTexture();
bool GetFogDisabled();
void SetFogDisabled(bool toggle);
// Removes the run-time fog override, so the user's own fog setting applies again.
void ClearFogOverride();
bool GetAutoFogToggleEnabled();
bool GetBloom();
bool GetReduceBloom();
float GetBloomIntensity();

bool GetNoclip();
bool GetInvulnerability();
bool GetSkipCutscene();
bool GetRestoreDashing();

bool GetEnableSecondaryGunFX();
bool GetShowGCCrosshair();
u32 GetGCCrosshairColor();

bool GetAutoArmAdjust();
bool GetToggleArmAdjust();
std::tuple<float, float, float> GetArmXYZ();

void UpdateHackSettings();

float GetSensitivity();
void SetSensitivity(float sensitivity);
float GetCursorSensitivity();
void SetCursorSensitivity(float sensitivity);
bool ScaleCursorSensitivity();
void SetScaleCursorSensitivity(bool scale);
float GetFov(Game game);
bool InvertedY();
void SetInvertedY(bool inverted);
bool InvertedX();
void SetInvertedX(bool inverted);
bool GetCulling();

bool HandleReticleLockOn();
void SetReticleLock(bool lock);

bool NewMapControlsEnabled();
void SetNewMapControls(bool new_controls);

enum CameraLock { Centre, Angle45, Unlocked };

void SetLockCamera(CameraLock lock);
CameraLock GetLockCamera();

bool CheckPitchRecentre();
bool ControllerMode();

std::pair<std::string, std::string> GetProfiles();
void ChangeControllerProfileAlt(std::string profile);

double GetHorizontalAxis();
double GetVerticalAxis();

std::tuple<bool, bool> GetMenuOptions();

AddressDB* GetAddressDB();
EmuVariableManager* GetVariableManager();

void SetMotd(std::string const& motd);
std::string GetMotd();

bool UsingRealWiimote();
}
