#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <iostream>
#include <fstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "misc.h"

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

int invoke()
{
    try {
        auto input = nlohmann::json::parse(std::cin);
        if (!input.contains("execute")) throw std::runtime_error("command is not specified");
        auto command = input["execute"].get<std::string>();
        auto arguments = input["arguments"];
        if (command == "echo") return echo(arguments);
        else if (command == "sleep") return sleep(arguments);
        else if (command == "system-status") return system_status(arguments);
        else throw std::runtime_error("Unknown command: " + command);
    }
    catch (const std::runtime_error& err) {
        std::cout << nlohmann::json({{"error", err.what()}});
        return 1;
    }
    catch (const nlohmann::json::parse_error& err) {
        std::cout << nlohmann::json({{"error", err.what()}});
        return 2;
    }
    return 0;
}

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
    nlohmann::json arguments;
    return system_status(arguments);
}
#endif
