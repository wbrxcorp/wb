#ifndef __MISC_H__
#define __MISC_H__

#include <string>

inline std::string human_readable(uint64_t size, double k = 1024.0)
{
    char buf[32];
    char au = 'K';

    float s = size / k;

    if (s >= k) {
        s /= k;
        au = 'M';
    }
    if (s >= k) {
        s /= k;
        au = 'G';
    }
    if (s >= k) {
        s /= k;
        au = 'T';
    }
    if (s >= k) {
        s /= k;
        au = 'P';
    }
    sprintf(buf, "%.1f%c", s, au);
    return std::string(buf);
}


#endif // __MISC_H__
