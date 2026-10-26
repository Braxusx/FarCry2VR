#pragma once
#include <cstdint>
#include <cstddef>
namespace fc2vr::menu {
// A native indexed record is usable only inside the constructed range, never
// merely because the backing allocation/spare slot is readable or non-null.
inline bool HasExtra(bool descriptorMatches, std::uint32_t count, bool recordMatches) {
    return descriptorMatches && count>4 && count<=64 && recordMatches;
}
struct Matches {std::size_t count=0,offset=0;};
inline Matches Find(const std::uint8_t* bytes,std::size_t size,const std::uint8_t* pattern,std::size_t length) {
    Matches result;
    if(!bytes || !pattern || !length || length>size)return result;
    for(std::size_t at=0;at<=size-length;++at) {
        std::size_t i=0;while(i<length && bytes[at+i]==pattern[i])++i;
        if(i==length){++result.count;result.offset=at;}
    }
    return result;
}
}
