// controls.h - motion controllers mapped onto the game's gamepad input (see controls.cpp).
#pragma once
#include <windows.h>

namespace controls {
void CancelPendingInput();   // game thread: cancel pending edges/pulses without synthesizing release actions
struct Config {
    int   enable = 1;          // MotionControls
    float rumble = 1.0f;       // ControllerRumble: scale of the game's rumble on the controllers (0 = off)
    int   stickPitch = 0;      // TurnStickPitch: 1 = right stick up/down also pitches the view
    int   turnFull = 1;        // TurnFullSpeed: any left/right push of the right stick turns at full speed
    float snapTurn = 45.0f;    // SnapTurn: degrees per right-stick push (0 = smooth turning)
    int   holdMs = 400;        // ButtonHoldMs: B held this long = next throwable instead of next weapon
    // holster zones, metres relative to the head (head-yaw frame)
    float shoulderFwd = -0.02f, shoulderUp = -0.20f;
    float hipUp = -0.45f;
    float chestUpMin = -0.55f, chestUpMax = -0.10f;
    int   logZones = 0;        // LogHolsterZones
    float sightOn = 0.25f, sightOff = 0.35f;
    int   throwArc = 1;        // ThrowArc: grip + trigger of a hand aims a throw along an arc, releasing throws
    int   throwArcRight = 1;   // ThrowArcRight: the right hand can do it too (right grip held blocks the gun trigger)
    int   monocular = 1;       // Monocular: right grip with the map up takes the monocular out / puts it back (v212)
    int   recenterHoldMs = 1000; // RecenterHoldMs: left stick held in this long starts the recenter countdown (v222, 0 = off)
    int   turretInvertY = 0;   // TurretInvertY: 1 = stick up lowers a mounted gun (v221)
    int   flashlight = 0;      // Flashlight: left hand at the left temple + left trigger switches a head-mounted light on / off (v220)
    float flashRadius = 0.13f; // FlashlightGestureRadius: how close (m) the left hand must be to the temple
    int   macheteTrigger = 0;  // MacheteTriggerAttack: 1 = the right trigger also does the game's machete attack (its animation sways the camera)
    int   sightNeedsGrip = 1;   // ScopeNeedsGrip: sights only with the left hand on the weapon   // SightOnDistance / SightOffDistance: right hand to head (m)
};
void Install(HMODULE dunia, const Config& c);
bool Active();   // VR controllers currently drive the game's gamepad
void SetLeftGripClaimed(bool c);
void PulseUse();
bool SightsUp();
bool MacheteSelected();
float RightTriggerValue();
bool MonocularOut();              // the monocular is out (the game's aim button held with the map up)
bool FlashlightOn();
bool TakeRecenterRequest();      // v222: the left stick was held in (once per hold)             // v220: the flashlight is switched on
bool MonocularAtFace();           // ... and held at the eye: scope view, the trigger tags       // the right controller's trigger (0..1), last frame
void SetMenuMode(bool on);
bool MapOpen();
void SetSightsAllowed(bool a);
int TakeSnap();                  // snap turns requested since the last call (+1 right, -1 left each)
void SetSnapStick(float x);      // right stick x sent to the game for a turn it must do itself (0 = none)
float SnapDegrees();
void SetTurnSpeed(float k);      // v224: smooth turning speed (1 = the game's full stick)
float TurnSpeed();
float TurnExtra();               // v224: extra turn the mod adds (stick direction x speed above 1)
void SetWatchActive();           // the safehouse watch is in its time-setting mode (call every frame it is)
bool WatchActive();
bool MountedWeapon();             // on a mounted gun / turret
bool MenuControls();              // the game's pause menu (in-game menu) action map is active
bool ItemControls();              // the game's own item / menu / cut-scene controls (watch, map, phone, menus, cinematic)
bool LoadingControls();           // no action map / the loading-screen map (405707ae)
bool MapControls();               // the game's map / GPS action map is up (also while answering the phone)
bool NormalControls();            // the game's normal on-foot action map is active (not phone, watch, map, menus)
int VehicleMode();
int ThrowAimHand();              // -1 none, 0 left, 1 right: that hand is aiming a throw (grip + trigger held)
int TakeThrowRelease();          // the aiming hand let go since the last call (1) - the arc decides whether to throw
void FireThrow();                // press the game's throw button
bool PhoneOut();                  // a phone call is up (see controls.cpp)
void ClearPhone();
int TakeRecapture();              // left grip + A: take the current weapon hold again
int TakeHolsterToggle();          // left grip + B was pressed (put the weapon away / bring it back)
int TakeReloadPress();            // the reload button was pressed since the last call
void SetRightHandFree(bool f);   // arms: nothing in the right hand (it may aim a throw)
void SetHandsAway(bool a);       // arms: the weapon is put away (left grip + B)
int TakeMapLift();               // the map button was pressed with the weapon away: lift the holster lock
int TakeWeaponSelect();           // a weapon was selected (holster grab or B)                // 0 on foot, 1 driving (driver seat), 2 other vehicle seat / mounted gun (from the game's action map)   // false: the sight button is not sent (unscoped weapons)
bool RightHandInHolsterZone();   // the right hand is in a weapon holster zone   // the map (left Y) is out, as far as the controls know   // sticks go to the mod menu, not the game
   // the last weapon slot chosen was the machete   // the weapon is held up to the face (iron sight / scope)   // press 'use' briefly (hand interaction)   // the left grip is holding the weapon (no 'use')
}
