#include "common/weapon_identity.h"
#include <cstdio>
#include <cstdlib>
#include <map>
using namespace fc2vr::weapons;
static void Check(bool condition) { if (!condition) { std::fputs("weapon identity failure\n",stderr); std::exit(1); } }
int main() {
    std::map<uintptr_t,uintptr_t> memory{{0x1024,0x2000},{0x2054,0x3000},{0x3000,0x4000},
                                      {0x5024,0x6000},{0x6054,0x7000},{0x7000,0x8000}};
    auto read=[&](uintptr_t address,uintptr_t& value) { auto i=memory.find(address); if(i==memory.end()) return false; value=i->second; return true; };
    Check(Inspect(0,0x4000,read).kind==Kind::None);
    auto machete=Inspect(0x1000,0x4000,read);
    Check(machete.MeleeFor(0x1000)); Check(!machete.MeleeFor(0x5000)); Check(!machete.MeleeFor(0));
    // Selecting a slot does not change identity until the engine equips it.
    auto pistol=Inspect(0x5000,0x4000,read);
    Check(pistol.kind==Kind::Other && !pistol.MeleeFor(0x5000));
    Check(!Inspect(0x9000,0x4000,read).MeleeFor(0x9000));
    Check(Inspect(0x1000,0,read).kind==Kind::Unknown);
    memory[0x2054]=0; Check(Inspect(0x1000,0x4000,read).kind==Kind::Unknown);
    memory[0x2054]=0x9000; Check(Inspect(0x1000,0x4000,read).kind==Kind::Unknown);
    // Address reuse must inspect the new native strategy, not a pointer cache.
    memory[0x2054]=0x3000; memory[0x3000]=0x8000;
    Check(!Inspect(0x1000,0x4000,read).MeleeFor(0x1000));
    std::puts("weapon identity: native changes, delayed equip, missing metadata and address reuse passed");
}
