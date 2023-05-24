#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <uuid/uuid.h>

#include <iostream>
#include <vector>
#include <future>
#include <ext/stdio_filebuf.h> // for __gnu_cxx::stdio_filebuf

#include <libsmartcols/libsmartcols.h>
#include <nlohmann/json.hpp>
#include <iniparser4/iniparser.h>
#include <ext2fs/ext2_fs.h>

#include "volume.h"
#include "vm.h"

static int systemctl(const std::string& action, const std::string& service, bool quiet = false)
{
    auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        if (quiet) {
            _exit(execlp("systemctl", "systemctl", "-q", getuid() == 0? "--system" : "--user", action.c_str(), service.c_str(), NULL));
        } else {
            _exit(execlp("systemctl", "systemctl", getuid() == 0? "--system" : "--user", action.c_str(), service.c_str(), NULL));
        }
    }
    int wstatus;
    waitpid(pid, &wstatus, 0);
    if (!WIFEXITED(wstatus)) throw std::runtime_error("systemctl command terminated abnormally");
    return WEXITSTATUS(wstatus);
}

static bool is_autostart(const std::string& vmname)
{
    return systemctl("is-enabled", "vm@" + vmname + ".service", true) == 0;
}

static bool is_running(const std::string& vmname)
{
    return systemctl("is-active", "vm@" + vmname + ".service", true) == 0;
}

static std::optional<std::string> get_volume_name_from_vm_name(const std::filesystem::path& vm_root, const std::string& vmname)
{
    auto vm_dir = vm_root / vmname;
    if (!std::filesystem::exists(vm_dir) || !std::filesystem::is_directory(vm_dir)) {
        throw std::runtime_error(vmname + " does not exist");
    }
    if (!std::filesystem::is_symlink(vm_dir)) return std::nullopt;
    auto symlink = std::filesystem::read_symlink(vm_dir);

    auto real_vm_dir = symlink.is_relative()? (vm_dir.parent_path() / std::filesystem::read_symlink(vm_dir)) : symlink;
    auto volume_dir = real_vm_dir.parent_path();
    auto volume_name = volume_dir.filename().string();
    if (volume_name[0] != '@') return std::nullopt; // not a volume path
    volume_name.replace(volume_name.begin(), volume_name.begin() + 1, ""); // remove '@'
    auto volume_dir_should_be = volume::get_volume_dir(vm_root, volume_name);
    if (!volume_dir_should_be) return std::nullopt;
    return (volume_dir_should_be == volume_dir)? std::make_optional(volume_name) : std::nullopt;
}

static std::optional<nlohmann::json> qga_execute_query(int fd, const nlohmann::json& query)
{
    auto message_str = query.dump() + '\n';
    if (write(fd, message_str.c_str(), message_str.length()) < 0) {
        throw std::runtime_error("Error sending message via socket");
    }
    std::string message;
    while (true) {
        struct pollfd pollfds[1];
        pollfds[0].fd = fd;
        pollfds[0].events = POLLIN;
        if (poll(pollfds, 1, 200) < 0) throw std::runtime_error("poll() failed");
        if (pollfds[0].revents & POLLIN) {
            char c;
            auto n = read(fd, &c, sizeof(c));
            if (n < 1) throw std::runtime_error("Error receiving message via socket");
            if (c == '\n') break;
            //else
            message += c;
        } else {
            return std::nullopt;
        }
    }
    return nlohmann::json::parse(message);
}

static void create_allocated_nocow_file(const std::filesystem::path& path, size_t size)
{
    auto fd = open(path.c_str(), O_CREAT|O_RDWR, S_IRUSR|S_IWUSR);
    if (fd < 0) throw std::runtime_error("Creating file with open() failed. Error createing data file.");
    int f = 0;
    if (ioctl(fd, EXT2_IOC_GETFLAGS, &f) == 0) {
        f |= FS_NOCOW_FL;
        ioctl(fd, EXT2_IOC_SETFLAGS, &f);
    }
    close(fd);
    fd = open(path.c_str(), O_RDWR);
    if (fd < 0) throw std::runtime_error("open() failed. Error createing data file.");
    if (fallocate(fd, 0, 0, size) < 0) {
        close(fd);
        throw std::runtime_error("fallocate() failed. Error createing data file. (err=" + std::string(strerror(errno)) + ")");
    }
    close(fd);
}

/*
static int restart(const std::vector<std::string>& args)
{
    argparse::ArgumentParser program(args[0]);
    program.add_argument("--console", "-c").help("Imeddiately connect to console").default_value(false).implicit_value(true);
    program.add_argument("--force", "-f").help("Force reset vm").default_value(false).implicit_value(true);
    program.add_argument("vmname").help("VM name");

    try {
        program.parse_args(args);
    }
    catch (const std::runtime_error& err) {
        std::cout << err.what() << std::endl;
        std::cout << program;
        return 1;
    }
    
    auto vmname = program.get<std::string>("vmname");
    if (!is_running(vmname)) {
        std::cerr << vmname << " is not running" << std::endl;
        return 1;
    }

    if (program.get<bool>("--force")) {
        with_qmp_session(vmname, [](int fd) {
            write(fd, "{ \"execute\": \"system_reset\"}\r\n");
            read_json_object(fd);
        }, [&vmname]() {
            throw std::runtime_error("QMP interface is not available for " + vmname);
        });
    } else {
        check_call({"systemctl", "restart", std::string("vm@") + vmname});
    }

    if (program.get<bool>("--console")) {
        return console(vmname.c_str());
    }

    return 0;
}

static int reboot(const std::vector<std::string>& args)
{
    argparse::ArgumentParser program(args[0]);
    program.add_argument("--console", "-c").help("Imeddiately connect to console").default_value(false).implicit_value(true);
    program.add_argument("vmname").help("VM name");

    try {
        program.parse_args(args);
    }
    catch (const std::runtime_error& err) {
        std::cout << err.what() << std::endl;
        std::cout << program;
        return 1;
    }
    
    auto vmname = program.get<std::string>("vmname");
    if (!is_running(vmname)) {
        std::cerr << vmname << " is not running" << std::endl;
        return 1;
    }

    with_qga(vmname, [](int fd) {
        write(fd, "{\"execute\":\"guest-shutdown\", \"arguments\":{\"mode\":\"reboot\"}}\r\n");
    }, [&vmname]() {
        throw std::runtime_error("Guest agent is not running on " + vmname + ".");
    });

    if (program.get<bool>("--console")) {
        return console(vmname.c_str());
    }

    return 0;
}

static int ping(const std::vector<std::string>& args)
{
    argparse::ArgumentParser program(args[0]);
    program.add_argument("vmname").help("VM name");

    try {
        program.parse_args(args);
    }
    catch (const std::runtime_error& err) {
        std::cout << err.what() << std::endl;
        std::cout << program;
        return 1;
    }
    
    auto vmname = program.get<std::string>("vmname");
    if (!is_running(vmname)) {
        std::cerr << vmname << " is not running" << std::endl;
        return 1;
    }

    with_qga(vmname, [](int fd) {
        write(fd, "{\"execute\":\"guest-ping\"}\r\n");
        auto tree = read_json_object(fd);
        if (!tree) std::runtime_error("Invalid response from VM");
        std::cout << "OK" << std::endl;

    }, [&vmname]() {
        throw std::runtime_error("Guest agent is not running on " + vmname + ".");
    });
    return 0;
}

static int status(const std::vector<std::string>& args)
{
    argparse::ArgumentParser program(args[0]);
    program.add_argument("vmname").help("VM name");
    try {
        program.parse_args(args);
    }
    catch (const std::runtime_error& err) {
        std::cout << err.what() << std::endl;
        std::cout << program;
        return 1;
    }

    exec({"systemctl", "status", std::string("vm@") + program.get<std::string>("vmname")});
    return 0;
}

static int journal(const std::vector<std::string>& args)
{
    argparse::ArgumentParser program(args[0]);
    program.add_argument("--follow", "-f").help("Act like 'tail -f'").default_value(false).implicit_value(true);
    program.add_argument("vmname").help("VM name");
    try {
        program.parse_args(args);
    }
    catch (const std::runtime_error& err) {
        std::cout << err.what() << std::endl;
        std::cout << program;
        return 1;
    }

    exec({"journalctl", program.get<bool>("--follow")? "-f" : "--pager", "-u", std::string("vm@") + program.get<std::string>("vmname")});
    return 0; // no reach here, though
}
*/

namespace vm {

int start(const std::string& vmname, bool console)
{
    if (is_running(vmname)) throw std::runtime_error(vmname + " is already running.");
    //else
    int rst = systemctl("start", "vm@" + vmname + ".service");
    if (rst == 0 && console) {
        return vm::console(vmname);
    }
    //else
    return rst;
}

int stop(const std::string& vmname, bool force, bool console)
{
    if (!is_running(vmname)) throw std::runtime_error(vmname + " is not running.");
    std::vector<const char*> argv = {"vm", "stop"};
    if (force) argv.push_back("-f");
    if (console) argv.push_back("-c");
    argv.push_back(vmname.c_str());
    argv.push_back(NULL);
    return execvp("vm", const_cast<char* const*>(argv.data()));
}

int restart(const std::string& vmname, bool force)
{
    if (force) {
        auto pid = fork();
        if (pid < 0) throw std::runtime_error("fork() failed");
        if (pid == 0) {
            stop(vmname, true, false); // it doesn't return as exevp() is called
        }
        //else
        waitpid(pid, NULL, 0);
    }
    return systemctl("restart", "vm@" + vmname + ".service");
}

int console(const std::string& vmname)
{
    if (!is_running(vmname)) throw std::runtime_error(vmname + " is not running.");
    std::vector<const char*> argv = {"vm", "console"};
    argv.push_back(vmname.c_str());
    argv.push_back(NULL);
    return execvp("vm", const_cast<char* const*>(argv.data()));
}

static int set_autostart(const std::string& vmname, bool on_off)
{
    return systemctl(on_off? "enable" : "disable","vm@" + vmname + ".service");
}

int autostart(const std::string& vmname, std::optional<bool> on_off)
{
    if (!on_off.has_value()) {
        std::cout << "autostart is " << (is_autostart(vmname)? "on" : "off") << std::endl;
        return 0;
    }
    //else
    return set_autostart(vmname, on_off.value());
}

int list(const std::filesystem::path& vm_root)
{
    struct VM {
        bool running = false;
        std::optional<uint16_t> cpu = std::nullopt;
        std::optional<uint32_t> memory = std::nullopt;
        std::optional<std::string> volume = std::nullopt;
        std::optional<bool> autostart = std::nullopt;
        std::optional<std::string> ip_address = std::nullopt;
    };

    std::map<std::string,VM> vms;
    if (std::filesystem::exists(vm_root) && std::filesystem::is_directory(vm_root)) {
        for (const auto& d : std::filesystem::directory_iterator(vm_root)) {
            if (!d.is_directory()) continue;
            auto name = d.path().filename().string();
            if (name[0] == '@' || name[0] == '.') continue; // directory starts with '@' is not VM but it's volume
            //else
            auto ini_path = d.path() / "vm.ini";
            auto ini = std::shared_ptr<dictionary>(std::filesystem::exists(ini_path)? iniparser_load(ini_path.c_str()) : dictionary_new(0), iniparser_freedict);
            uint16_t cpu = iniparser_getint(ini.get(), ":cpu", 0);
            uint32_t memory = iniparser_getint(ini.get(), ":memory", 0);
            vms[name] = {
                .cpu = cpu > 0? std::make_optional(cpu) : std::nullopt,
                .memory = memory > 0? std::make_optional(memory) : std::nullopt,
                .volume = get_volume_name_from_vm_name(vm_root, name),
                .autostart = is_autostart(name)
            };
        }
    }

    auto fd = memfd_create("show", 0);
    if (fd < 0) throw std::runtime_error("memfd_create() failed");

    __gnu_cxx::stdio_filebuf<char> filebuf(fd, std::ios::in); // this cleans up fd in dtor

    auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        _exit(execlp("vm", "vm", "show", NULL));
    }
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        throw std::runtime_error("subprocess exited with error");
    }
    lseek(fd, 0, SEEK_SET);
    std::istream f(&filebuf);
    auto json = nlohmann::json::parse(f);
    std::map<std::string,std::future<std::optional<std::string>>> threads;
    for (const auto& entry: json) {
        auto vmname = entry["name"];
        vms[vmname].running = true;
        vms[vmname].cpu = entry["cpus"].get<uint16_t>();
        vms[vmname].memory = entry["memory"].get<uint64_t>() / 1024 / 1024;
        auto qga = entry.contains("qga")? std::make_optional(std::filesystem::path(entry["qga"].get<std::string>())) : std::nullopt;
        // communicate via qga to get ipv4 address
        if (qga.has_value()) { 
            threads[vmname] = std::async([](const std::filesystem::path& qga) -> std::optional<std::string> {
                struct sockaddr_un sockaddr;
                memset(&sockaddr, 0, sizeof(sockaddr));
                auto sock = socket(AF_UNIX, SOCK_STREAM, 0);
                if (sock < 0) throw std::runtime_error("socket() failed");
                sockaddr.sun_family = AF_UNIX;
                strcpy(sockaddr.sun_path, qga.c_str());
                // TODO: qga socket should be locked
                if (connect(sock, (const struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0) {
                    close(sock);
                    return std::nullopt;
                }
                auto res = qga_execute_query(sock, nlohmann::json({ {"execute", "guest-network-get-interfaces"} }));
                if (!res.has_value() || !res.value().contains("return")) return std::nullopt;
                for (auto& interface : res.value()["return"]) {
                    if (interface["name"] == "lo") continue;
                    //else
                    for (auto& ipaddress : interface["ip-addresses"]) {
                        if (ipaddress["ip-address-type"] != "ipv4") continue;
                        //else
                        return std::string(ipaddress["ip-address"]);
                    }
                }
                return std::nullopt;
            }, qga.value());
        }
    }

    for (auto& [vmname, thread] : threads) {
        vms[vmname].ip_address = thread.get();
    }

    std::shared_ptr<libscols_table> table(scols_new_table(), scols_unref_table);
    if (!table) throw std::runtime_error("scols_new_table() failed");
    scols_table_new_column(table.get(), "RUNNING", 0.1, SCOLS_FL_RIGHT);
    scols_table_new_column(table.get(), "NAME", 0.1, 0);
    scols_table_new_column(table.get(), "VOLUME", 0.1, 0);
    scols_table_new_column(table.get(), "AUTOSTART", 0.1, SCOLS_FL_RIGHT);
    scols_table_new_column(table.get(), "CPU", 0.1, SCOLS_FL_RIGHT);
    scols_table_new_column(table.get(), "MEMORY", 0.1, SCOLS_FL_RIGHT);
    scols_table_new_column(table.get(), "IP ADDRESS", 0.1, SCOLS_FL_RIGHT);
    auto sep = scols_table_new_line(table.get(), NULL);
    scols_line_set_data(sep, 0, "-------");
    scols_line_set_data(sep, 1, "-------------");
    scols_line_set_data(sep, 2, "---------");
    scols_line_set_data(sep, 3, "---------");
    scols_line_set_data(sep, 4, "---");
    scols_line_set_data(sep, 5, "-------");
    scols_line_set_data(sep, 6, "---------------");

    for (const auto& i:vms) {
        auto line = scols_table_new_line(table.get(), NULL);
        if (!line) throw std::runtime_error("scols_table_new_line() failed");
        scols_line_set_data(line, 0, i.second.running? "*" : "");
        scols_line_set_data(line, 1, i.first.c_str());
        scols_line_set_data(line, 2, i.second.volume.value_or("-").c_str());
        scols_line_set_data(line, 3, i.second.autostart.has_value()? (i.second.autostart.value()? "yes":"no") : "-");
        const auto& cpu = i.second.cpu;
        scols_line_set_data(line, 4, cpu.has_value()? std::to_string(cpu.value()).c_str() : "-");
        const auto& memory = i.second.memory;
        scols_line_set_data(line, 5, memory.has_value()? std::to_string(memory.value()).c_str() : "-");
        const auto& ip_address = i.second.ip_address;
        scols_line_set_data(line, 6, ip_address.value_or("-").c_str());
    }
    scols_print_table(table.get());

    return 0;
}

int create(const std::filesystem::path& vm_root, const std::string& vmname, const CreateOptions& options/* = {}*/)
{
    // check if vmname is RFC952/RFC1123 compliant
    if (vmname.length() > 63) throw std::runtime_error("VM name must be 63 characters or less");
    if (vmname[0] == '-' || vmname[vmname.length() - 1] == '-') throw std::runtime_error("VM name must not start or end with '-'");
    if (vmname.find("--") != std::string::npos) throw std::runtime_error("VM name must not contain consecutive '-'");
    if (vmname.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
        throw std::runtime_error("VM name must contain only alphanumeric characters and '-'");
    if (vmname.find_first_not_of("0123456789") == std::string::npos) throw std::runtime_error("VM name must not be all digits");

    auto vm_dir = vm_root / vmname;
    if (std::filesystem::exists(vm_dir)) {
        throw std::runtime_error(vmname + " already exists");
    }

    auto [real_vm_dir,symlink] = options.volume? [&vm_root,&vmname](auto& volume) {
        auto volume_dir = volume::get_volume_dir(vm_root, volume);
        if (!volume_dir.has_value()) throw std::runtime_error("Volume " + volume + " does not exist or offline");
        //else
        auto real_vm_dir = volume_dir.value() / vmname;
        if (std::filesystem::exists(real_vm_dir)) throw std::runtime_error(real_vm_dir.string() + " already exists");
        //else
        return std::make_pair(real_vm_dir, std::make_optional(std::filesystem::path("@" + volume) / vmname));
    }(options.volume.value()) : std::make_pair(vm_dir, std::nullopt);

    try {
        auto fs_dir = real_vm_dir / "fs";
        std::filesystem::create_directories(fs_dir);

        if (options.system_file.has_value()) {
            std::filesystem::copy_file(options.system_file.value(), real_vm_dir / "system");
        }

        auto vm_ini = real_vm_dir / "vm.ini";
        {
            std::ofstream f(vm_ini);
            if (options.memory) f << "memory=" << std::to_string(*options.memory) << std::endl;
            if (options.cpu) f << "cpu=" << std::to_string(*options.cpu) << std::endl;
        }

        if (options.data_partition) {
            create_allocated_nocow_file(real_vm_dir / "data", *options.data_partition * 1024LL * 1024 * 1024/*GiB*/);
        }

        if (symlink) {
            std::filesystem::create_directory_symlink(*symlink, vm_dir);
        }
    }
    catch (...) {
        std::filesystem::remove_all(real_vm_dir);
        throw;
    }

    return 0;
}

int _delete(const std::filesystem::path& vm_root, const std::string& vmname)
{
    auto vm_dir = vm_root / vmname;
    if (!std::filesystem::exists(vm_dir) || !std::filesystem::is_directory(vm_dir)) {
        throw std::runtime_error(vmname + " does not exist");
    }

    auto [real_vm_dir,volume_dir] = std::filesystem::is_symlink(vm_dir)? [&vm_root](const auto& vm_dir) {
        auto symlink = std::filesystem::read_symlink(vm_dir);
        auto real_vm_dir = symlink.is_relative()? (vm_dir.parent_path() / std::filesystem::read_symlink(vm_dir)) : symlink;
        auto volume_dir = real_vm_dir.parent_path();
        auto volume_name = volume_dir.filename().string();
        if (volume_name[0] != '@') throw std::runtime_error(volume_dir.string() + " is not a volume path");
        volume_name.replace(volume_name.begin(), volume_name.begin() + 1, ""); // remove '@'
        auto volume_dir_should_be = volume::get_volume_dir(vm_root, volume_name);
        if (!volume_dir_should_be.has_value()) {
            throw std::runtime_error("Volume " + volume_name + " does not exist");
        }
        if (volume_dir_should_be != volume_dir) {
            throw std::runtime_error("Symlink " + vm_dir.string() + "(points " + real_vm_dir.string() + ") does not point VM dir right under volume");
        }
        return std::make_pair(real_vm_dir, std::make_optional(volume_dir));
    }(vm_dir) : std::make_pair(vm_dir, std::nullopt);

    if (is_running(vmname)) throw std::runtime_error(vmname + " is running");

    // disable corresponding systemctl service
    set_autostart(vmname, false);

    if (is_symlink(vm_dir)) std::filesystem::remove(vm_dir);  // remove symlink

    // move vm real dir to trash
    uuid_t uuid;
    char uuid_str[37];
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, uuid_str);
    auto trash_dir = (volume_dir? *volume_dir : vm_root) / ".trash";
    std::filesystem::create_directories(trash_dir);
    std::filesystem::rename(real_vm_dir, trash_dir / (vmname + '.' + uuid_str));

    return 0;
}

} // namespace vm

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
    create_allocated_nocow_file("nocow.bin", 1024*1024);
    //std::cout << nlohmann::json({"execute", "guest-network-get-interfaces"}) << std::endl;
    //return _main(argc, argv);
}
#endif
