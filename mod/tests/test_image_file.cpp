#include "gameaddr.h"
#include <cstdio>
#include <cstdarg>
#include <filesystem>
void LogWrite(const char* format,...) { va_list args; va_start(args,format); std::vprintf(format,args); va_end(args); std::puts(""); }
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    fc2vr::image::Identity id;
    if(!gameaddr::InspectFile(std::filesystem::path(argv[1]).c_str(),id)) { std::puts("file identity unavailable"); return 1; }
    char hex[65]; fc2vr::image::Hex(id.sha,hex);
    const auto selected=fc2vr::image::Select(id);
    std::printf("image-file measured=%d profile=%d bytes=%llu sha256=%s timestamp=%08x imageSize=%08x\n",id.measured,(int)selected,(unsigned long long)id.fileSize,hex,id.pe.timestamp,id.pe.imageSize);
    return selected==fc2vr::image::Build::Steam?0:1;
}
