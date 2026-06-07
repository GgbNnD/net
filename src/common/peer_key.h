#pragma once

#include <string>
#include <map>
#include <mutex>

namespace PeerKey {

void store(const std::string& ip, const std::string& shared_secret);
std::string get(const std::string& ip);

} // namespace PeerKey
