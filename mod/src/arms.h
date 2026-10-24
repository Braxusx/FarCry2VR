// arms.h - weapon/arms follow the right controller; player shots follow its aim ray (see arms.cpp).
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include "common/pose_frame.h"
#include "common/pose_authority.h"

namespace arms {
struct Config {
    int bodyClock = 1;            // HandsOnBodyClock (v231): hands follow the body's movement since the frame was published
    int roomScale = 0;            // RoomScale (v231): real steps move the player (the body follows the head)
    float roomLeash = 0.10f;      // RoomScaleLeash: the head may move this far (m) from the body before the body follows (leaning)
    float roomMaxSpeed = 4.0f;    // RoomScaleMaxSpeed: m/s the body may be moved at
    float roomRadius = 0.30f;     // RoomScaleWallDistance: the body stops this far (m) short of a wall
    int weaponInHand = 1;         // WeaponInHand
    int aimFromController = 1;    // AimFromController
    int dump = 1;                 // DumpSkeleton: log the player skeleton's bone list once
    float handOfs[3] = { 0, 0, 0 };   // HandOffsetX/Y/Z: right-hand position relative to the controller grip (m; right, up, back)
    int ik = 1;                   // ArmIK: shoulders from the head, elbows solved
    int freeLeftHand = 1;         // FreeLeftHand: left hand follows the left controller when not holding the weapon
    float shoulderSide = 0.17f, shoulderDown = 0.22f, shoulderBack = 0.06f;   // ShoulderSide/Down/Back (m from the eyes)
    int noIdleBreak = 1;          // NoIdleAnimation: never play the idle weapon-inspect animation
    float leftHandRoll = 45.0f;   // LeftHandRoll: free left hand turned clockwise about the controller axis (deg)
    float leftHandYaw = 30.0f;    // LeftHandYaw: free left hand turned inward about the controller's up axis (deg)
    float leftForearmTwist = 1.0f;   // LeftForearmTwist: share of LeftHandRoll carried by the forearm
    float upperArmTwist = 0.5f;   // UpperArmTwist: share of the forearm's turn the upper arm follows
    int   steadyHands = 1;    // SteadyHands: animations (sprint sway, gun pulled in near walls) don't turn the hands
    int   armsIndependent = 1;   // ArmsIndependent: upper arms keep the body orientation; the free left forearm follows the left controller
    float mapAngle = 0.0f, mapYaw = 0.0f, mapRoll = 0.0f;   // MapPitch/MapYaw/MapRoll: hands holding the map and GPS (deg)
    float macheteRoll = 0.0f;     // MacheteRoll: machete hand turned about the controller's pointing axis (deg, + = clockwise seen from behind)
    float macheteYaw = 15.0f;     // MacheteYaw: machete angled inward about the controller's up axis (deg)
    float macheteAngle = 60.0f;   // MacheteAngle: machete (and hand) tilted up from the controller's pointing direction (deg)
    int handGrab = 1;             // HandGrab: grab pickups / use things with a free hand's grip
    float grabRadius = 0.35f;     // GrabRadius: search radius around the hand (m)
    float grabBoxShrink = 0.25f;  // GrabBoxShrink: the object's bounding box is shrunk by this share before measuring the reach
    float grabReach = 0.06f;      // GrabReach: a loose object is grabbed when the hand is this close to it (m)
    float pickupReach = 0.20f;    // PickupReach: hand to a pickup item (m)
    int   grabPhysics = 1;        // GrabPhysics: loose physics objects can be picked up and thrown
    float grabMaxMass = 25.0f;    // GrabMaxMass: heaviest loose object a hand can lift (kg)
    float grabMaxSize = 0.8f;     // GrabMaxSize: largest loose object (Havok object radius, m)
    float bodyRadius = 0.30f;     // BodyRadius: held objects are kept this far from the body's centre line (m)
    int   climbing = 0;           // Climbing: grip buildings to climb them
    int   climbMode = 0;          // ClimbMode: 0 set position, 1 velocity, 2 controller out of the world (Home cycles)
    float climbReach = 0.10f;     // ClimbReach: hand to the surface (m)
    float freePitch = 0, freeYaw = 0, freeRoll = 0;
    int   phoneDiag = 0;          // PhoneDiag: log the arm/skeleton diagnostic during calls and other scripted scenes
    float phonePitch = 0, phoneYaw = 0, phoneRoll = 0;   // PhonePitch/Yaw/Roll: the hands during a phone call (scripted scenes), instead of the free-hand angles   // FreeHandPitch/Yaw/Roll: empty hands (left hand off the weapon, right hand with nothing equipped)
    int   weaponRollFix = 0;     // WeaponRollFix: turn the hand when the game leaves the weapon turned in it (after the phone)
    int   macheteHits = 1;      // MacheteHits: swinging the machete into an NPC lands the game's machete hit
    float macheteSpeed = 2.5f;   // MacheteSwingSpeed: m/s the blade tip must move
    float macheteCooldown = 0.5f; // MacheteCooldown: s before the same NPC can be hit again
    int   seatWeapon = 1;       // SeatWeapon: put a weapon the game re-attached turned/shifted back in its recorded place in the hand
    int   macheteSound = 1;      // MacheteSwingSound: the game's machete swoosh plays on a fast VR swing
    int   noColorFilter = 0;  // NoColorFilter: 1 = no colour grading / desaturation ("brown filter") - v223
    int   noRimLight = 0;     // NoRimLight: 1 = no rim lighting on characters - v223
    float flashRange = 30.0f, flashInner = 18.0f, flashOuter = 32.0f;   // v220 Flashlight: FlashlightRange (m), FlashlightInnerAngle / OuterAngle (deg)
    float flashBright = 1.0f, flashForward = 0.10f;   // FlashlightBrightness (colour scale), FlashlightForward (m in front of the head)
    float macheteSoundSpeed = 6.0f; // MacheteSoundSpeed: blade-tip m/s that counts as a swing for the sound
    uint32_t macheteSnd[2] = { 0x004B2686, 0x004B2689 };   // MacheteSoundIds: the sounds the game plays on its own machete attack
    float bladeLen = 0.55f;      // MacheteBladeLength: m from the hand
    int   freezeReload = 1;     // FreezeReload: the arms and weapon keep their pose during reload animations (sound still plays)
    int   contactPush = 1;      // ContactPush: a moving hand / held weapon shoves loose objects and bodies it touches
    float pushMinSpeed = 0.3f;   // PushMinSpeed: m/s the hand must move
    float pushRadius = 0.06f;    // PushRadius: contact distance (m) from the object's box / a body part
    float pushScale = 1.0f;      // PushScale: object speed = hand speed x this
    float pushMass = 40.0f;      // PushMass: kg up to which an object takes the full speed (heavier: less)
    float pushWeaponLen = 0.7f;  // PushWeaponLen: m of the held weapon that pushes
    int   npcGrab = 1;          // NpcGrab: grab NPCs / bodies by a body part (living ones are knocked down while held)
    float npcReach = 0.25f;      // NpcReach: hand to body / standing NPC distance for a grab (m)
    float npcGain = 0.6f;        // NpcPullGain: share of the gap to the hand closed per frame
    float npcMaxSpeed = 10.0f;   // NpcMaxSpeed: m/s
    float npcRagdollTime = 2.5f;
    float npcActiveWeight = 1.0f; // NpcActiveWeight: how much of the ragdoll the standing NPC's mesh shows (0..1)
    float npcStiffness = 1.0f;   // NpcStiffness: how strongly a held standing NPC's body holds its animated pose (x the game's gains)
    int   npcActive = 1;         // NpcActive: 1 = a grabbed NPC stays standing (its ragdoll follows its animation) until pulled / shoved over
    float npcKnockPull = 1.6f;   // NpcKnockPull: m the held limb is pulled from where its body wants it, for 0.25 s = knocked down
    float npcKnockDown = 0.35f;  // NpcKnockDown: m the held limb is pulled below where its body wants it = pulled to the ground
    float npcLeadGain = 8.0f;    // NpcLeadGain: how quickly a held standing NPC steps after a sideways pull (1/s)
    float npcLeadSpeed = 4.5f;   // NpcLeadSpeed: m/s at most
    float npcKnockSpeed = 3.0f;  // NpcKnockSpeed: hand speed (m/s) when letting go that shoves the NPC over // NpcRagdollTime: a released living NPC stays limp this long (s)
    float throwRange = 1.5f;     // ThrowRange: throw distance x this (speed x its square root)
    float throwSpeed = 0.0f;     // ThrowSpeed: m/s of an arc throw (0 = the throwable's own strength)
    float watchStickSign = 1.0f; // WatchStickSign: -1 reverses the right stick on the safehouse watch
    float climbTurnSpeed = 120.0f; // ClimbTurnSpeed: right-stick turning on the wall, if the game doesn't turn the body itself (deg/s)
    int   climbHolster = 1;       // ClimbHolster: put the weapon away while climbing, back in hand on the roof
    int   gripInteract = 0;       // GripInteract: 1 = an empty grip press also interacts (E); 0 = X interacts
    float climbGrace = 1.5f;      // ClimbGrace: after letting go of every hold, hang this long (s); any grip catches a new hold
    float mantleBelowHead = 0.55f; // MantleBelowHead: a hold this far below your eyes pulls you over the edge (m)
    float throwScale = 1.3f;      // ThrowScale: hand speed at release x this = throw speed
    float throwMaxSpeed = 25.0f;  // ThrowMaxSpeed (m/s)
    float useReach = 0.9f;
    float mouthReach = 0.18f;     // MouthReach: food/water consumed this close to the mouth (m)        // UseReach: hand to the game's interaction point (m)
    float backpackFwd = 0.0f, backpackUp = -0.25f;   // BackpackZoneFwd/Up: over the shoulder, relative to the head (m)
    uint32_t grabMask = 0x4A7;    // GrabQueryMask: physics query layers
    float twoHandMaxSpan = 0.65f; // the animation's hands further apart than this = one-handed weapon (pistol)
};
// Per-frame data from the render thread. "ref" = the recentred tracking frame (x right, y up, -z forward),
// scaled to game units; game is the exact effective frame also used by the eyes.
struct Frame {
    bool valid = false, handValid = false;
    bool handAuthority = false, leftAuthority = false, headAuthority = false;
    fc2vr::authority::Stamp source{};
    void* weapon = nullptr;   // instance identity, not a model/cache generation lease
    fc2vr::tracking::GameBasis game{};
    uint64_t frameSequence = 0;   // diagnostic application-frame stamp, not a freshness lease
    float bonePub[3] = {}; bool bonePubValid = false;   // v231: the Camera bone when this frame was published (body clock)
    float camRows[3][3];          // RAW native camera rows (incl. pitch): bone-axis calibration only
    float aimO[3], aimR[3], aimU[3], aimB[3];   // right controller aim pose (ref)
    float gripO[3];               // right controller grip position (ref)
    bool leftValid = false, twoHand = false, headValid = false;
    float lSqueeze = 0, rSqueeze = 0;
    bool machete = false, map = false;   // machete in hand; map/GPS out
    float laimO[3], laimR[3], laimU[3], laimB[3], lgripO[3];   // left controller (ref)
    float headO[3], headF[3];     // head position and forward (ref)
};
typedef bool (*PatchFn)(uintptr_t target, const uint8_t* expect, size_t n, void* hook, void** tramp);
void Install(uintptr_t base, const Config& c, PatchFn patch);
void SetWeaponKey(uint32_t k);     // current weapon type id (HUD reticle element name), 0 = unknown
void SetIniPath(const char* p);    // fc2vr.ini: saved weapon holds [Holds]
void SubmitFrame(const Frame& f);
// Observation only: these stamps do not grant freshness or pose authority.
struct FrameUsage {
    bool haveApplied = false, haveInteraction = false;
    uint64_t appliedSequence = 0, interactionSequence = 0;
    uint32_t appliedCalls = 0, interactionCalls = 0, appliedThread = 0, interactionThread = 0;
};
bool ReadFrameUsage(FrameUsage& out);
bool CamBonePos(float out[3]);
bool BodyClock(); void SetBodyClock(bool on);
long CamBoneSeq();
bool DiagHand(float applyW[3], float liveW[3], float hand[3], float target[3], long* seq);   // v231 jitter diagnostic                                  // v231: skeleton updates so far (diagnostic)   // v231 (F4 test toggle)
void SetMonoCams(const float game[16], const float posed[16]);
bool MonoTarget();
void SetHeadWorld(const float pos[3], const float dir[3], const float up[3]);
void SetRoomScaleOffset(const float off[3], bool valid);   // v231 render thread: the head's horizontal offset from the body, game space
bool TakeRoomScaleMoved(float out[3]);                    // v231 render thread: how far the body was moved since the last call (game space)   // v220: the head in game world space (flashlight)
bool MonoEquipped();   // v218: the game has the monocular out (in the hand)   // v217: something taggable is in the monocular's ring (the game's binoculars target)
void MonoRenderFix(const float game[16], const float posed[16]);   // v216: move the drawn monocular with the scope camera (same frame)   // v215: the game's render camera world and the head-aimed scope camera world
bool PlayerState(float pos[3], float* heading);   // player entity position and the game's heading (radians)
void RequestGiveThrowables();   // test: fill grenades and molotovs to the maximum (adds them if missing)
// throwing arc for display: points in the ref frame (game units), landing surface normal (ref), ok = a throwable is ready
int GetThrowArc(float (*pts)[3], int maxPts, bool& land, float normal[3], bool& ok);
// where the left hand grips the weapon (ref frame); oneHanded = pistol-style weapon
bool Foregrip(float out[3], bool& oneHanded);
float LastHandSpan();
struct RayContext { fc2vr::authority::Stamp source{}; void* weapon = nullptr; };
void SetScopeRay(const float o[3], const float d[3], const RayContext& context);   // world; retains original source age/owner
void SetUseCallback(void (*cb)());
void SetMachete(float angle, float yaw, float roll);
void GetMachete(float& angle, float& yaw, float& roll);
void SetMapHands(float angle, float yaw, float roll);
void GetMapHands(float& angle, float& yaw, float& roll);
void SetFreeHands(float angle, float yaw, float roll);
void GetFreeHands(float& angle, float& yaw, float& roll);
void SetPhoneHands(float angle, float yaw, float roll);
void GetPhoneHands(float& angle, float& yaw, float& roll);
void* CurrentWeapon();   // the player's current weapon object (identity only)                     // called (game thread) when a hand uses the interaction target
// the weapon's sight line (ref frame: origin, unit direction); false if not updated recently
bool SightRay(float o[3], float d[3], RayContext* context = nullptr);
float AimDistance();
bool ProjectileReloadActive();     // within the 3.5 s after a projectile weapon shot (its reload)
void NotifyClip(int clip, int pouch);         // the HUD clip count changed (player's weapon)               // game units along the sight ray to what the weapon points at (0 = unknown)
bool CameraRay(float o[3], float d[3]);   // the game camera's line in the reference frame (mounted guns aim along it)
void Stats(LONG& applied, LONG& shots);
}
