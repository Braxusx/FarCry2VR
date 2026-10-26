#include "common/menu_guard.h"
#include <cstdlib>
int main() {
    for(unsigned count=0;count<100;++count)
        for(unsigned descriptor=0;descriptor<2;++descriptor)
            for(unsigned record=0;record<2;++record)
                if(fc2vr::menu::HasExtra(descriptor,count,record)!=(descriptor&&record&&count>=5&&count<=64)) return EXIT_FAILURE;
    // Readable spare memory, including all-zero or plausible pointer-looking
    // contents, never promotes record 4 into the four constructed records.
    if(fc2vr::menu::HasExtra(true,4,true) || fc2vr::menu::HasExtra(true,4,false))return EXIT_FAILURE;
    if(!fc2vr::menu::HasExtra(true,5,true))return EXIT_FAILURE;
    const std::uint8_t bytes[]={1,2,3,1,2,4};
    const std::uint8_t one[]={2,3},duplicate[]={1,2},missing[]={5};
    if(fc2vr::menu::Find(bytes,6,one,2).count!=1 || fc2vr::menu::Find(bytes,6,one,2).offset!=1)return EXIT_FAILURE;
    if(fc2vr::menu::Find(bytes,6,duplicate,2).count!=2 || fc2vr::menu::Find(bytes,6,missing,1).count)return EXIT_FAILURE;
    if(fc2vr::menu::Find(bytes,6,one,7).count || fc2vr::menu::Find(bytes,6,one,0).count || fc2vr::menu::Find(nullptr,6,one,2).count)return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
