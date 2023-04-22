#ifndef __VOLUME_H__
#define __VOLUME_H__

#include <optional>
#include <filesystem>

namespace volume {
    std::optional<std::pair<std::filesystem::path,std::string/*fstype*/>> get_source_device_from_mountpoint(const std::filesystem::path& path);
    std::optional<std::filesystem::path> get_volume_dir(const std::filesystem::path& vm_root, const std::string& volume_name);

    int add(const std::filesystem::path& vm_root, const std::string& name, const std::filesystem::path& device);
    int remove(const std::filesystem::path& vm_root, const std::string& name);
    int scan(const std::filesystem::path& vm_root);

    struct ListOptions {
        bool online_only = false;
        bool names_only = false;
    };
    int list(const std::filesystem::path& vm_root, const ListOptions& options = {});
    int snapshot(const std::filesystem::path& vm_root, const std::string& volume_name);
    int backup(const std::filesystem::path& vm_root);
    int clean(const std::filesystem::path& vm_root, const std::optional<std::string>& volume_name);
}

#endif
