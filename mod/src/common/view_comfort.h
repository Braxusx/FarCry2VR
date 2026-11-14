#pragma once
#include "pose_frame.h"
#include <cmath>

namespace fc2vr::tracking {
struct ViewComfortResult {
    GameBasis basis{};
    bool active=false, yawSuppressed=false, positionSuppressed=false;
};

// Presentation only. The caller must first match the ORIGINAL native camera to
// its BodyPose, retain that native identity, and pass the resulting sample here.
// No live pawn reads, native angle writes, shot changes or input integration.
class ShotViewComfort {
    static constexpr float pi=3.14159265358979323846f;
    static constexpr double minimumRecoveryMs=500, settledMs=100, maximumRecoveryMs=5000;
    bool haveOwner=false, haveBaseline=false, active=false;
    std::uintptr_t skeleton=0, graphics=0, pawnData=0, weapon=0;
    std::uint32_t desiredStance=0, effectiveStance=0;
    std::uint64_t lastSequence=0, lastShot=0;
    double lastTime=0, latestShotTime=0, settledSince=-1;
    Vec3 baselineOffset{}; // right, forward, up in native LookAngles heading
    float baselineYaw=0;
    ViewComfortResult cached{};

    static bool Finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
    static float Wrap(float angle) {
        return std::remainder(angle,2*pi);
    }
    static Vec3 Root(const BodyPose& scene) { return {scene.root[12],scene.root[13],scene.root[14]}; }
    static GameBasis Heading(float yaw,Vec3 position) {
        GameBasis result;
        result.F={std::cos(yaw),std::sin(yaw),0};
        result.R={result.F.y,-result.F.x,0};result.P=position;
        return result;
    }
    static Vec3 Offset(const GameBasis& raw,const BodyPose& scene,const GameBasis& source) {
        const auto offset=Subtract(raw.P,Root(scene));
        return {DotProduct(offset,source.R),DotProduct(offset,source.F),offset.z};
    }
    static bool Valid(const GameBasis& raw,const BodyPose& scene) {
        const auto& native=scene.nativeView;
        if(!scene.valid || !scene.sequence || !scene.skeleton || !scene.graphics || !native.valid
            || !native.pawnData || !native.weapon || !std::isfinite(scene.timeMs)
            || !std::isfinite(native.timeMs) || scene.timeMs<=0
            || std::fabs(scene.timeMs-native.timeMs)>5
            || !Finite(raw.P) || !Finite(raw.R) || !Finite(raw.U) || !Finite(raw.F)
            || !Finite(scene.camera)) return false;
        for(float value:scene.root) if(!std::isfinite(value)) return false;
        for(float value:native.angles) if(!std::isfinite(value)) return false;
        if(native.shotSequence && (!std::isfinite(native.shotTimeMs) || native.shotTimeMs<=0
            || native.shotTimeMs>scene.timeMs+2)) return false;
        const auto error=Subtract(raw.P,scene.camera);
        if(DotProduct(error,error)>0.0000011f) return false; // preserve the 1 mm native match
        // Desired/effective LookAngles agreed throughout the measured GOG run.
        // A different native control blend needs its own evidence, not a guess.
        return std::fabs(Wrap(native.angles[2]-native.angles[8]))<=0.000872665f;
    }
    bool SameOwner(const BodyPose& scene) const {
        const auto& native=scene.nativeView;
        return haveOwner && skeleton==scene.skeleton && graphics==scene.graphics
            && pawnData==native.pawnData && weapon==native.weapon
            && desiredStance==native.desiredStance && effectiveStance==native.effectiveStance;
    }
    void SeedOwner(const BodyPose& scene) {
        const auto& native=scene.nativeView;
        haveOwner=true;skeleton=scene.skeleton;graphics=scene.graphics;
        pawnData=native.pawnData;weapon=native.weapon;
        desiredStance=native.desiredStance;effectiveStance=native.effectiveStance;
        lastShot=native.shotSequence;
    }
public:
    void Reset() { *this=ShotViewComfort{}; }

    ViewComfortResult Update(const GameBasis& raw,const BodyPose& scene,bool eligible) {
        ViewComfortResult result{raw};
        if(!eligible || !Valid(raw,scene)
            || scene.nativeView.desiredStance!=scene.nativeView.effectiveStance) {
            Reset();return result;
        }
        const auto& native=scene.nativeView;
        if(!SameOwner(scene) || scene.sequence<lastSequence || scene.timeMs<lastTime
            || (lastTime>0 && scene.timeMs-lastTime>250) || native.shotSequence<lastShot) {
            Reset();SeedOwner(scene);
        }
        if(scene.sequence==lastSequence) return active ? cached : result;

        // Static Euler evidence and 240 captured deliberate-turn frames establish
        // this conversion: native LookAngles Z rotates a +Y forward direction.
        const float yaw=Wrap(native.angles[2]+pi*.5f);
        const auto source=Heading(yaw,Root(scene));
        const auto offset=Offset(raw,scene,source);
        const float residual=Wrap(std::atan2(raw.F.y,raw.F.x)-yaw);
        const double age=native.shotSequence ? scene.timeMs-native.shotTimeMs : maximumRecoveryMs+1;
        const bool newShot=native.shotSequence>lastShot;
        if(newShot && haveBaseline && age>=-2 && age<250) {
            active=true;latestShotTime=native.shotTimeMs;settledSince=-1;
        }
        lastShot=native.shotSequence;

        if(active) {
            const auto difference=Subtract(offset,baselineOffset);
            const float distance2=DotProduct(difference,difference);
            const float angle=std::fabs(Wrap(residual-baselineYaw));
            const double sinceShot=scene.timeMs-latestShotTime;
            // A stance/posture change missed by the enum, incompatible camera,
            // or stale shot must never freeze a native transition in the headset.
            if(distance2>0.08f*0.08f || angle>15*pi/180 || sinceShot>maximumRecoveryMs) {
                active=false;haveBaseline=false;settledSince=-1;
            } else {
                const bool settled=sinceShot>=minimumRecoveryMs
                    && distance2<=0.001f*0.001f && angle<=0.05f*pi/180;
                if(!settled) settledSince=-1;
                else if(settledSince<0) settledSince=scene.timeMs;
                if(settled && scene.timeMs-settledSince>=settledMs) active=false;
                if(active) {
                    const auto position=Add(Add(Add(Root(scene),Scale(source.R,baselineOffset.x)),
                        Scale(source.F,baselineOffset.y)),Vec3{0,0,baselineOffset.z});
                    result.basis=Heading(Wrap(yaw+baselineYaw),position);
                    result.active=result.yawSuppressed=result.positionSuppressed=true;
                }
            }
        }
        // Never learn a baseline from the first frame of an already active shot.
        // Between confirmed shots retain the exact native view, including turns.
        if(!active && age>=minimumRecoveryMs && std::fabs(residual)<=15*pi/180) {
            baselineOffset=offset;baselineYaw=residual;haveBaseline=true;
        }
        lastSequence=scene.sequence;lastTime=scene.timeMs;cached=result;
        return result;
    }
};
} // namespace fc2vr::tracking
