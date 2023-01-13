#ifndef __MISC_H__
#define __MISC_H__

#include <string>

std::string human_readable(uint64_t size, double k = 1024.0);
bool wayland_ping(bool wait);
int generate_rdp_cert();

#endif // __MISC_H__
