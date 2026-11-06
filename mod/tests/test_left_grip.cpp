#include "common/left_grip.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

struct Matrix {float m[16]{};};
static int checks=0;
static void Check(bool ok,const char* label){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1);}}
static Matrix Pose(float yaw,float pitch,float x=0,float y=0,float z=0){
    const float c=std::cos(yaw),s=std::sin(yaw),a=std::cos(pitch),b=std::sin(pitch);
    return {{c,s,0,0,-s*a,c*a,b,0,s*b,-c*b,a,0,x,y,z,1}};
}
static Matrix Mul(const Matrix& a,const Matrix& b){Matrix r{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)r.m[i*4+j]+=a.m[i*4+k]*b.m[k*4+j];return r;}
static Matrix Mirror(const Matrix& a,int axis){Matrix r;fc2vr::left_grip::Mirror(a.m,axis,r.m);return r;}
static void Point(const Matrix& a,const float* p,float* out){for(int j=0;j<3;++j)out[j]=p[0]*a.m[j]+p[1]*a.m[4+j]+p[2]*a.m[8+j]+a.m[12+j];}
static float Distance(const float* a,const float* b){float d=0;for(int i=0;i<3;++i)d+=(a[i]-b[i])*(a[i]-b[i]);return std::sqrt(d);}
static float Error(const Matrix& a,const Matrix& b){float d=0;for(int i=0;i<16;++i)d=std::fmax(d,std::fabs(a.m[i]-b.m[i]));return d;}
static Matrix WeaponMirror(const Matrix& a,const Matrix& muzzle){Matrix out;Check(fc2vr::left_grip::MirrorWeapon(a.m,muzzle.m,out.m),"finite muzzle center-plane reflection");return out;}
static float Det(const Matrix& a){return a.m[0]*(a.m[5]*a.m[10]-a.m[6]*a.m[9])-a.m[1]*(a.m[4]*a.m[10]-a.m[6]*a.m[8])+a.m[2]*(a.m[4]*a.m[9]-a.m[5]*a.m[8]);}
int main(){
    // Observed neutral wrists on Steam Dunia 6353936a...862765. The dominant
    // left hand uses bone-Z/grip-X reflection, not the weapon's support pose.
    const Matrix rightNeutral{{-.052f,-.180f,-.982f,0,-.999f,.010f,.051f,0,.001f,.984f,-.180f,0,0,0,0,1}};
    const Matrix leftNeutral{{.051f,-.179f,-.982f,0,.999f,.010f,.050f,0,.001f,-.984f,.180f,0,0,0,0,1}};
    Check(Error(Mirror(rightNeutral,2),leftNeutral)<.002f,"mirrored dominant wrist matches anatomical neutral left basis");
    Check(Error(Mirror(leftNeutral,2),rightNeutral)<.002f,"mirrored support wrist matches anatomical neutral right basis");
    int rejectsOld=0;
    for(int degree=-720;degree<=720;degree+=5){
        const float angle=degree*.01745329252f;
        const Matrix grip=Pose(angle,.3f,.4f,1.3f,-.2f),hand=Pose(.2f,-.1f);
        Matrix gun=Pose(.1f,.05f);const Matrix muzzleFrame=Pose(.4f,.2f,.035f,.08f,-.3f);
        const float palmRight[3]={.03f,.04f,-.02f},palmLeft[3]={.03f,.04f,.02f};
        float handle[3];for(int k=0;k<3;++k)handle[k]=muzzleFrame.m[12+k]+.1f*muzzleFrame.m[4+k]-.07f*muzzleFrame.m[8+k];
        float contact[3],handlePoint[3];Point(hand,palmRight,contact);Point(gun,handle,handlePoint);
        for(int k=0;k<3;++k)gun.m[12+k]+=contact[k]-handlePoint[k];
        const Matrix mirroredWeapon=WeaponMirror(gun,muzzleFrame);
        const Matrix leftHand=Mul(Mirror(hand,2),grip),leftGun=Mul(mirroredWeapon,grip);
        const Matrix rightMuzzle=Mul(muzzleFrame,gun),leftMuzzle=Mul(muzzleFrame,mirroredWeapon);
        float mirroredPoint[3]={-rightMuzzle.m[12],rightMuzzle.m[13],rightMuzzle.m[14]};
        Check(Distance(leftMuzzle.m+12,mirroredPoint)<.00001f,"muzzle center mirrors without model-pivot parallax");
        float mirroredForward[3]={-rightMuzzle.m[4],rightMuzzle.m[5],rightMuzzle.m[6]};
        Check(Distance(leftMuzzle.m+4,mirroredForward)<.00001f,"muzzle forward is mirrored, never reversed as a lateral model axis");
        float palm[3],weapon[3],oldWeapon[3];Point(leftHand,palmLeft,palm);Point(leftGun,handle,weapon);Point(Mul(gun,grip),handle,oldWeapon);
        Check(Distance(palm,weapon)<.00001f,"gun handle stays in mirrored left palm");
        if(Distance(palm,oldWeapon)>.01f)++rejectsOld;
        Check(Error(WeaponMirror(WeaponMirror(gun,muzzleFrame),muzzleFrame),gun)<.00001f,"mirroring is reversible");
        Check(std::fabs(Det(leftHand)-1)<.00001f && std::fabs(Det(leftGun)-1)<.00001f,"proper rotations preserve mesh winding");
        const Matrix turn=Pose(-.4f,.2f),carried=Mul(grip,turn);
        Check(Error(Mul(leftGun,turn),Mul(WeaponMirror(gun,muzzleFrame),carried))<.00001f,"two-hand turn follows mirroring in the carried grip frame");
        const Matrix supportPose=Pose(.9f,-.7f,.1f,.02f,-.3f);
        const Matrix rightSupport=Mul(Mirror(supportPose,2),carried);
        Check(std::fabs(Det(rightSupport)-1)<.00001f,"support wrist conversion preserves a proper rotation");
        Check(Error(Mirror(Mirror(supportPose,2),2),supportPose)<.00001f,"support pose conversion is reversible");
        Check(Error(rightSupport,Mul(hand,carried))>.2f,"support orientation is distinguishable from translated trigger wrist regression");
        float camera[3]={0,0,-1},native[3]={.04f,0,-1},barrel[3]={std::sin(angle),.2f,-std::cos(angle)},out[3];
        fc2vr::left_grip::Unit(native);fc2vr::left_grip::Unit(barrel);
        Check(fc2vr::left_grip::Spread(camera,native,barrel,out),"spread rotation is finite");
        const float original=native[0]*camera[0]+native[1]*camera[1]+native[2]*camera[2];
        const float rotated=out[0]*barrel[0]+out[1]*barrel[1]+out[2]*barrel[2];
        Check(std::fabs(original-rotated)<.00002f,"native spread cone is preserved across full turns");
        Check(fc2vr::left_grip::Spread(camera,camera,barrel,out) && Distance(out,barrel)<.00001f,"zero-spread shot follows rendered barrel");
        const float muzzle[3]={-.25f,1.3f,-.4f};
        float target[3];for(int k=0;k<3;++k)target[k]=muzzle[k]+barrel[k]*.15f;
        float closest[3];for(int k=0;k<3;++k)closest[k]=muzzle[k]+out[k]*.15f;
        Check(Distance(closest,target)<.00001f,"muzzle ray intersects close target without camera parallax");
        float side[3]={-barrel[2],0,barrel[0]};fc2vr::left_grip::Unit(side);
        for(int k=0;k<3;++k)closest[k]+=side[k]*.25f;
        Check(Distance(closest,target)>.24f,"regression rejects camera-origin ray at close range");
    }
    Check(rejectsOld>200,"regression rejects unmirrored right-grip gun placement");
    float old[3]={0,0,-1},opposite[3]={0,0,1},out[3];
    Check(fc2vr::left_grip::Spread(old,old,opposite,out) && Distance(out,opposite)<.00001f,"antiparallel spread basis");
    for(float epsilon:{.001f,.0001f,.00001f}){
        float nearOpposite[3]={epsilon,0,1};fc2vr::left_grip::Unit(nearOpposite);
        Check(fc2vr::left_grip::Spread(old,old,nearOpposite,out) && Distance(out,nearOpposite)<.00001f,"near-antiparallel spread follows barrel exactly");
    }
    std::printf("PASS: %d left-palm contact, proper rotation, two-hand and close-range spread checks\n",checks);
}
