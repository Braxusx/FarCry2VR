#include "common/projectile_aim.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>
static void Check(bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
static float Distance(const float* a,const float* b){float s=0;for(int i=0;i<3;++i)s+=(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(s);}
int main(){
    using fc2vr::projectile::Aim;using fc2vr::projectile::Resolve;
    const float native[3]={.35f,1.4f,.5f},barrel[3]={-.25f,1.3f,-.4f};
    int cases=0;
    for(int degree=-720;degree<=720;degree+=5)for(float range:{.02f,.15f,5.f,300.f}){
        const float a=degree*.01745329252f;float direction[3]={std::sin(a),0,-std::cos(a)},target[3];
        for(int i=0;i<3;++i)target[i]=barrel[i]+direction[i]*range;
        Aim result;Check(Resolve(true,native,barrel,direction,target,result),"left projectile resolves");
        Check(Distance(result.origin,barrel)==0,"projectile leaves rendered left muzzle, not native hand/back");
        Check(Distance(result.direction,direction)<.00001f,"left projectile follows rendered barrel through full turns / close range");
        Check(result.hitAllowed,"forward target can remain a hit");++cases;
    }
    const float direction[3]={0,0,-1},forwardTarget[3]={-.25f,1.3f,-10},behind[3]={-.25f,1.3f,.5f};Aim result;
    Check(Resolve(false,native,barrel,direction,forwardTarget,result) && Distance(result.origin,native)==0,"right-hand native spawn remains unchanged");
    Check(Resolve(true,native,barrel,direction,behind,result) && !result.hitAllowed && Distance(result.direction,direction)==0,"behind-barrel target cannot force a backwards impact");
    Check(Resolve(true,native,barrel,direction,barrel,result) && !result.hitAllowed,"zero-distance left target uses forward ray without forced hit");
    Check(!Resolve(false,native,barrel,direction,native,result),"right zero-distance fallback remains native");
    const float zero[3]={0,0,0};Check(!Resolve(true,native,barrel,zero,forwardTarget,result),"invalid forward rejected");
    const float bad[3]={std::numeric_limits<float>::quiet_NaN(),0,0};
    Check(!Resolve(true,native,bad,direction,forwardTarget,result),"invalid owned muzzle rejected");
    Check(!Resolve(true,native,barrel,direction,bad,result),"invalid target rejected");
    Check(Resolve(true,bad,barrel,direction,forwardTarget,result),"left mode does not inherit an invalid native origin");
    std::printf("PASS: %d full-turn/near-target projectile cases; left muzzle origin, behind-target rejection, right preservation and finite gates\n",cases);
}
