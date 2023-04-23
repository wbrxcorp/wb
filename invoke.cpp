/**
 * @file invoke.cpp
 * @author Walbrix Corporation 
 */
#include <unistd.h>
#include <sched.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <iostream>
#include <fstream>
#include <thread>
#include <map>

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include "install.h"
#include "misc.h"

static const uint64_t least_size = 1024 * 1024 * 1024 * 8LL/*8GB*/;

static std::optional<std::string> get_interface_name_with_default_gateway()
{
    std::ifstream routes("/proc/net/route");
    if (!routes) return std::nullopt;
    std::string line;
    if (!std::getline(routes, line)) return std::nullopt;// skip header
    while (std::getline(routes, line)) {
        std::string ifname;
        auto i = line.begin();
        while (i != line.end() && !isspace(*i)) ifname += *i++;
        if (i == line.end()) continue; // no destination
        while (i != line.end() && isspace(*i)) i++; // skip space(s)
        std::string destination;
        while (i != line.end() && !isspace(*i)) destination += *i++;

        if (destination == "00000000") return ifname;
    }
    return std::nullopt; // not found
}

static std::optional<std::string> get_ipv4_address()
{
    auto ifname = get_interface_name_with_default_gateway();
    if (!ifname || ifname.value().length() >= IFNAMSIZ) return std::nullopt;
    //else
    auto s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return std::nullopt;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, ifname.value().c_str());
    if (ioctl(s, SIOCGIFADDR, &ifr) < 0) return std::nullopt;
    return inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
}

static std::optional<std::string> get_cpu_model()
{
    std::ifstream f("/proc/cpuinfo");
    if (!f) return std::nullopt;
    std::string s;
    while (std::getline(f, s)) {
        if (s.starts_with("model name\t: ")) return s.substr(13);
    }
    return std::nullopt;
}

static std::optional<std::tuple<uint64_t,uint64_t,uint64_t>> get_cpu_clock() {
    uint64_t cpuinfo_cur_freq;
    uint64_t cpuinfo_min_freq = 0L, cpuinfo_max_freq = 0L;
    try {
        {
            std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq");
            if (!f) {
                if(f.is_open()) f.close();
                f.open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
                if (!f) return std::nullopt;
            }
            f >> cpuinfo_cur_freq;
        }
        {
            std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
            if (f) {
                f >> cpuinfo_min_freq;
            }
        }
        {
            std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
            if (f) {
                f >> cpuinfo_max_freq;
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }
    return std::make_tuple(cpuinfo_cur_freq, cpuinfo_min_freq, cpuinfo_max_freq);
}

static std::optional<std::pair<uint64_t,uint64_t> > get_memory_capacity() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo) return std::nullopt;
    std::string line;
    uint64_t available = 0, total = 0;
    while (std::getline(meminfo, line)) {
        std::string column_name;
        auto i = line.begin();
        while (i != line.end() && *i != ':') column_name += *i++;
        if (i == line.end()) continue; // unexpected EOL
        i++; // skip ':'
        while (i != line.end() && isspace(*i)) i++; // skip space(s)
        std::string value, unit;
        while (i != line.end() && !isspace(*i)) value += *i++;
        while (i != line.end() && isspace(*i)) i++; // skip space(s)
        if (i != line.end()) {
            while (i != line.end()) unit += *i++;
        }
        if (column_name == "MemTotal" && unit == "kB") total = std::stoul(value) * 1024;
        else if (column_name == "MemAvailable" && unit == "kB") available = std::stoul(value) * 1024;
        if (total > 0 && available > 0) break;
    }
    if (total == 0 || available == 0) return std::nullopt;
    //else
    return std::make_pair(available, total);
}

namespace invoke {

int sleep(const nlohmann::json& arguments)
{
    auto seconds = arguments.get<uint8_t>();
    sleep(seconds);
    std::cout << nlohmann::json({{"return", true}});
    return 0;
}

int echo(const nlohmann::json& arguments)
{
    std::cout << arguments;
    return 0;
}

int system_status(const nlohmann::json& arguments)
{
    struct utsname u;
    if (uname(&u) < 0) throw std::runtime_error("uname(2) failed");
    std::string serial_number = u.nodename;
    std::string kernel_version = u.release;
    std::optional<std::string> ip_address = get_ipv4_address();
    std::optional<std::string> cpu_model = get_cpu_model();
    uint32_t cpus = std::thread::hardware_concurrency();
    auto clock = get_cpu_clock();
    auto memory = get_memory_capacity();

    nlohmann::json result;
    result["serial_number"] = serial_number;
    result["kernel_version"] = kernel_version;
    if (ip_address) result["ip_address"] = *ip_address;
    if (cpu_model) result["cpu_model"] = *cpu_model;
    result["cpus"] = cpus;
    if (clock) {
        result["clock"] = {
            {"current", std::get<0>(*clock)},
            {"min", std::get<1>(*clock)},
            {"max", std::get<2>(*clock)}
        };
    }
    if (memory) {
        result["memory"] = {
            {"unused", memory->first},
            {"total", memory->second}
        };
    }
    result["kvm"] = std::filesystem::exists("/dev/kvm");

    std::cout << nlohmann::json({
        {"return", result}
    });

    return 0;
}

int install(const nlohmann::json& arguments)
{
    auto disk = arguments.get<std::string>();
    if (getuid() > 0) {
        // mock install
        if (disk == "/dev/error") throw std::runtime_error("Installation failed");
        auto message = [](const std::string& msg) {
            std::cout << "MESSAGE:" << msg << std::endl;
            usleep(200000);
        };
        auto progress = [](const double fraction) {
            std::cout << "PROGRESS:" << fraction << std::endl;
            usleep(200000);
        };
        message("Creating partitions...");
        message("Creating partitions done.");
        progress(0.03);
        message("Formatting boot partition with FAT32");
        progress(0.05);
        message("Mouning boot partition...");
        message("Done");
        progress(0.07);
        message("Installing UEFI bootloader");
        message("Installing BIOS bootloader");
        message("This system will be UEFI-only as this disk cannot be treated by BIOS");
        progress(0.09);
        message("Creating boot configuration file");
        progress(0.10);
        message("Copying system file");
        message("Unmounting boot partition...");
        message("Done");
        progress(0.90);
        message("Constructing data area");
        message("Formatting partition for data area with BTRFS...");
        message("Done");
        progress(1.00);
        return 0;
    }
    //else
    if (unshare(CLONE_NEWNS) < 0) throw std::runtime_error("unshare(CLONE_NEWNS) failed");
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        throw std::runtime_error("Changing root filesystem propagation failed");
    }
    auto rst = install::install(disk, least_size, "/run/initramfs/boot/system.img", 
            {}, // grub vars
            [](double fraction){
                std::cout << "PROGRESS:" << fraction << std::endl;
            },
            [](const std::string& message){
                std::cout << "MESSAGE:" << message << std::endl;
            }
        );
    return rst? 0 : 1;
}

int get_usable_disks_for_install(const nlohmann::json&)
{
    auto mock_disks = []() -> std::map<std::string,install::Disk> {
        std::map<std::string,install::Disk> disks;
        disks["/dev/hoge"] = install::Disk {
            .name = "hoge",
            .model = "MY SUPER DUPER DISK",
            .size = 1024LL * 1024 * 1024 * 512,
            .tran = "SATA",
            .log_sec = 512
        };
        disks["/dev/error"] = install::Disk {
            .name = "error",
            .model = "MY BROKEN DISK",
            .size = 1024LL * 1024 * 1024 * 128,
            .tran = "NVMe",
            .log_sec = 512
        };
        disks["/dev/fuga"] = install::Disk {
            .name = "fuga",
            .model = std::nullopt,
            .size = 1024LL * 1024 * 1024 * 64,
            .tran = std::nullopt,
            .log_sec = 512
        };
        ::sleep(1);
        return disks;
    };

    auto disks = getuid() == 0? install::enum_usable_disks(least_size) : mock_disks();

    nlohmann::json result;
    result["return"] = nlohmann::json::array();
    for (const auto& [name,disk]:disks) {
        auto obj = nlohmann::json({
            {"name", name},
            {"size", disk.size},
            {"log_sec", disk.log_sec}
        });
        if (disk.model) obj["model"] = *disk.model;
        if (disk.tran) obj["tran"] = *disk.tran;
        result["return"].push_back(obj);
    }

    std::cout << result;
    return 0;
}

/**
 * @brief Detect timezone using ip-api.com
 * @return 0 on success, 1 on failure
 * prints JSON to stdout
 */
int detect_timezone(const nlohmann::json&)
{
    std::shared_ptr<CURL> curl(curl_easy_init(), curl_easy_cleanup);
    curl_easy_setopt(curl.get(), CURLOPT_URL, "http://ip-api.com/json");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto& buffer = *static_cast<std::string*>(userdata);
        buffer.append(ptr, size * nmemb);
        return size * nmemb;
    });
    std::string buffer;
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
    auto res = curl_easy_perform(curl.get());
    if (res != CURLE_OK) throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
    auto json = nlohmann::json::parse(buffer);
    if (!json.contains("timezone")) throw std::runtime_error("timezone not found in response");
    std::cout << nlohmann::json({
        {"return", json["timezone"]}
    });
    return 0;
}

static std::map<std::string, 
    std::pair<std::function<int(const nlohmann::json&)>,bool/*stream_response*/>> commands = {
    {"echo", {echo, false}},
    {"sleep", {sleep, false}},
    {"system-status", {system_status, false}},
    {"install", {install, true}},
    {"get-usable-disks-for-install", {get_usable_disks_for_install, false}},
    {"detect-timezone", {detect_timezone, false}}
};

int invoke()
{
    bool stream_response = false;
    try {
        auto input = nlohmann::json::parse(std::cin);
        if (!input.contains("execute")) throw std::runtime_error("command is not specified");
        auto command = input["execute"].get<std::string>();
        auto arguments = input["arguments"];
        if (!commands.contains(command)) throw std::runtime_error("Unknown command: " + command);
        //else
        auto& [func,_stream_response] = commands[command];
        stream_response = _stream_response;
        return func(arguments);
    }
    catch (const std::exception& err) {
        if (stream_response) {
            std::cout << "ERROR:" << err.what() << std::endl;
        } else {
            std::cout << nlohmann::json({{"error", err.what()}});
        }
    }
    return 1;
}

} // namespace invoke

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
    nlohmann::json arguments;
    return system_status(arguments);
}
#endif
