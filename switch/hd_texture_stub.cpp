// The HD texture pack is a downloadable 7z archive; its implementation needs the 7-Zip SDK, which is
// a desktop dependency the Switch build does not carry. The pack is optional, so this reports it as
// absent and ignores downloads. The game renders with its original Wii textures.
#include <string>

#include "hd_texture_pack.h"

namespace HdTexturePack {

Status Get() {
    Status status;
    status.state = State::NotInstalled;
    status.progress = -1.f;
    status.message = "HD texture packs are not available on this platform";
    return status;
}

void Download() {}

bool TakeJustInstalled() { return false; }

bool Enabled() { return false; }

void SetEnabled(bool on) { (void)on; }

} // namespace HdTexturePack
