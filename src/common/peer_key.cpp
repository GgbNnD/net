#include "common/peer_key.h"

namespace PeerKey {

static std::map<std::string, std::string> s_secrets;
static std::mutex s_mutex;

void store(const std::string& ip, const std::string& shared_secret) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_secrets[ip] = shared_secret;
}

std::string get(const std::string& ip) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_secrets.find(ip);
    return (it != s_secrets.end()) ? it->second : "";
}

} // namespace PeerKey
