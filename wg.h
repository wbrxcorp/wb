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
}

#endif