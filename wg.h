#ifndef __WG_H__
#define __WG_H__
#include <string>
#include <vector>
#include <optional>

namespace wg {
    int genkey(bool force);
    int pubkey(bool qrcode);
    int getconfig(bool accept_ssh_key);
    int notify(const std::string& uri);
    int ping(bool success_if_not_active, uint16_t count, bool verbose);
}

#endif