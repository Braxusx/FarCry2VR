#pragma once
// Which Dunia.dll build is loaded, and translation of the mod's engine addresses. Every address in the mod's source is
// the Steam build's (Dunia.dll TimeDateStamp 0x4AAE9636, Sep 2009) virtual address at the preferred base 0x10000000.
// On the GOG build (0x49FB4BF6, May 2009) the same functions/data sit elsewhere; Xlate maps them through a table that
// was built offline by matching code bytes (relocations and branch displacements wildcarded) between the two DLLs.
#include <cstdint>
#include "common/image_identity.h"
namespace gameaddr {
enum Build { Unknown = 0, Steam = 1, GOG = 2 };
int Detect();                        // exact file identity + bounded mapped/file x86 PE agreement; unknown refuses
bool InspectFile(const wchar_t* path,fc2vr::image::Identity& out); // read-only, shared with offline tests
const char* Name();
uint32_t Xlate(uint32_t steamVa);    // Steam VA -> this build's VA (preferred base); unmapped on GOG -> 0 (logged)
}
