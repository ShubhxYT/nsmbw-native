// Discord rich presence is a desktop feature whose implementation needs <sys/un.h>, which Horizon
// does not have. The runtime calls it unconditionally, so this provides the same interface as a
// no-op: presence simply never appears.
#include <string>

#include "discord_presence.h"

namespace DiscordPresence {

void Initialize(const std::string& basicClientId, const std::string& basicTitle) {
    (void)basicClientId;
    (void)basicTitle;
}

void SetClient(const std::string& clientId) { (void)clientId; }

void SetActivity(Activity activity) { (void)activity; }

void Reset() {}

void Shutdown() {}

} // namespace DiscordPresence
