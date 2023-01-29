#ifndef __INSTALL_H__
#define __INSTALL_H__

#include <filesystem>
#include <optional>

namespace install {
    struct Disk {
        std::string name;
        std::optional<std::string> model;
        uint64_t size;
        std::optional<std::string> tran;
        uint16_t log_sec;
    };
    std::map<std::string,Disk> enum_usable_disks(uint64_t least_size);

    bool install(const std::filesystem::path& disk, uint64_t least_size, 
        const std::filesystem::path& system_img,
        const std::map<std::string,std::string>& grub_vars,
        std::function<void(double)> progress = [](auto){}, 
        std::function<void(const std::string&)> message = [](auto){});
    int install(const std::filesystem::path& disk, const std::filesystem::path& system_image,
        bool text_mode, bool installer);
    int show_usable_disks();
    int create_install_media(const std::filesystem::path& disk, const std::filesystem::path& system_image);
}
#endif // __INSTALL_H__