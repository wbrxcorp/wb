#ifndef __VM_H__
#define __VM_H__
#include <optional>
#include <string>
#include <filesystem>

namespace vm {
    int start(const std::string& vmname, bool console);
    int stop(const std::string& vmname, bool force, bool console);
    int restart(const std::string& vmname, bool force);
    int console(const std::string& vmname);
    int autostart(const std::string& vmname, std::optional<bool> on_off);
    int list(const std::filesystem::path& vm_root);

    struct CreateOptions {
        const std::optional<std::string>& volume = std::nullopt;
        std::optional<uint32_t> memory = std::nullopt;
        std::optional<uint16_t> cpu = std::nullopt;
        std::optional<uint32_t> data_partition = std::nullopt;
        const std::optional<std::filesystem::path>& system_file = std::nullopt;
    };
    int create(const std::filesystem::path& vm_root, const std::string& vmname, const CreateOptions& options = {});
    int _delete(const std::filesystem::path& vm_root, const std::string& vmname);
}

#endif // __VM_H__