#include <filesystem>
#include <optional>

#ifndef __INSTALL_H__
#define __INSTALL_H__
namespace install {
    int install(const std::filesystem::path& disk, const std::filesystem::path& system_image,
        bool text_mode, bool installer);
    int show_usable_disks();
    int create_install_media(const std::filesystem::path& disk, const std::filesystem::path& system_image);
}
#endif // __INSTALL_H__