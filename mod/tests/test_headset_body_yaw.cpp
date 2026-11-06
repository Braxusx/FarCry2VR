#include "common/headset_body_yaw.h"
#include <cstdio>
#include <cstdlib>
using namespace fc2vr::body_yaw;
static int checks=0;
static void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
static bool Near(float a,float b){return std::fabs(Wrap(a-b))<0.0001f;}
static State Ready(){State s;s.Owner(123);float out=0;Check(s.Render(0.4f,1.2f,true,out),"initial basis");return s;}
int main(){
 float yaw=0;
 Check(HeadYaw(-1,0,yaw) && Near(yaw,Pi/2),"left turn sign");
 Check(HeadYaw(1,0,yaw) && Near(yaw,-Pi/2),"right turn sign");
 Check(!HeadYaw(0,0,yaw),"near vertical head rejected");
 Check(!HeadYaw(NAN,1,yaw),"nonfinite head rejected");
 Check(Near(Wrap(179*Pi/180-(-179*Pi/180)),-2*Pi/180),"shortest wrap");
 State state=Ready();Sample sample{123,state.reference,1,1000,Pi/2,true,true};
 const float first=state.Plan(sample,1001);Check(Near(first,Pi/4),"bounded first step");
 state.Commit(first);Check(state.Plan(sample,1001)==0,"one transfer per source sample");
 sample.sequence++;const float second=state.Plan(sample,1001);state.Commit(second);
 Check(Near(state.transferred,Pi/2),"body catches up to 90 degrees");
 sample.sequence++;Check(state.Plan(sample,1001)==0,"stationary head does not spin body");
 float corrected=0;state.Render(0.4f,1.2f+Pi/2,true,corrected);
 Check(Near(corrected,0.4f),"body catchup excluded even if raw camera is old");
 Check(Near(corrected+sample.headYaw,0.4f+Pi/2),"view turns once");
 // Native thumbstick yaw must remain in both body and rendered view.
 state.Render(0.4f+0.3f,1.2f+Pi/2+0.3f,true,corrected);
 Check(Near(corrected,0.7f),"manual thumbstick turn preserved");
 state.Render(1.0f,0,false,corrected);Check(Near(corrected,1.0f-Pi/2),"vehicle/menu compensation retained");
 for(int kind=0;kind<8;++kind){
  auto s=Ready();Sample bad{123,s.reference,1,1000,1,true,true};
  if(kind==0)bad.owner=456;
  if(kind==1)bad.reference++;
  if(kind==2)bad.tracked=false;
  if(kind==3)bad.onFoot=false;
  if(kind==4)bad.receivedMs=899;
  if(kind==5)bad.receivedMs=1001;
  if(kind==6)bad.headYaw=NAN;
  if(kind==7)s.refused=true;
  Check(s.Plan(bad,1000)==0,"unsafe/stale source cannot turn body");
 }
 const auto oldReference=sample.reference;state.Reset();
 Check(state.reference!=oldReference && state.transferred==0 && !state.calibrated,"recenter resets compensation and calibration");
 Check(state.Plan(sample,1001)==0,"pre-recenter sample rejected");
 state=Ready();state.Commit(1);state.Owner(456);Check(state.transferred==0 && !state.calibrated,"pawn change cannot retain transfer");
 // Long physical turns, looking back, and native-camera lag at every step.
 state=Ready();float native=1.2f;
 for(int i=1;i<=1440;++i){
  const float h=Wrap(i*Pi/180);Sample a{123,state.reference,(unsigned)i,1000,h,true,true};
  float d=state.Plan(a,1000);native=Wrap(native+d);state.Commit(d);
  state.Render(0.4f,native,true,corrected);
  Check(Near(corrected,0.4f),"four full turns preserve world basis");
  Check(Near(state.transferred,h),"native body tracks physical yaw through wrap");
 }
 std::printf("headset body yaw: PASS (%d checks)\n",checks);
}
