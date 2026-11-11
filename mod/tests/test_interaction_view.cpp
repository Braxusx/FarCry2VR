#include "common/interaction_view.h"
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <initializer_list>
using namespace fc2vr::interaction;
static void Check(bool ok) { if(!ok) std::exit(1); }
int main() {
    for(auto d: {fc2vr::tracking::Vec3{0,1,0},{1,0,0},{-1,0,0},{0,0,1},{0,-1,0},{.6f,.8f,0},
                 {.004f,-.99999f,.002f},{-.0001f,-1,.0003f}}) {
        auto ray=MakeRay({3,2,1},d,true);float q[4];Check(Rotation(ray,q));
        // Rotate +Y with the emitted quaternion; must agree with the requested view.
        float x=2*(q[0]*q[1]-q[3]*q[2]);
        float y=1-2*(q[0]*q[0]+q[2]*q[2]);
        float z=2*(q[1]*q[2]+q[3]*q[0]);
        Check(std::fabs(x-ray.direction.x)<1e-5f && std::fabs(y-ray.direction.y)<1e-5f && std::fabs(z-ray.direction.z)<1e-5f);
    }
    Check(!MakeRay({0,0,0},{0,1,0},false).valid);
    Check(!MakeRay({0,0,0},{0,0,0},true).valid);
    auto nan=std::numeric_limits<float>::quiet_NaN();
    Check(!MakeRay({nan,0,0},{0,1,0},true).valid);
    Check(!MakeRay({0,0,0},{0,nan,0},true).valid);
    std::puts("interaction view: physical turn, pitch, antiparallel and invalid input passed");
}
