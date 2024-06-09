#include <pwd.h>
#include <unistd.h>

#include <argparse/argparse.hpp>

#include "vm.h"
#include "volume.h"
#include "install.h"
#include "wg.h"
#include "misc.h"
#include "invoke.h"

static const char* VERSION = "20240609";

static bool is_root_user()
{
    return (getuid() == 0);
}

static void must_be_root()
{
    if (!is_root_user()) throw std::runtime_error("You must be a root user.");
}

static std::filesystem::path user_home_dir()
{
    const auto home = getenv("HOME");
    if (home) return home;
    //else
    auto  pw = getpwuid(getuid());
    return pw->pw_dir;
}

static std::filesystem::path vm_root()
{
    return is_root_user()? "/var/vm" : user_home_dir() / "vm";
}

class Command : public argparse::ArgumentParser {
    typedef std::function<void(argparse::ArgumentParser&)> SetupFunc;
    typedef std::function<int(const argparse::ArgumentParser&)> RunFunc;
    SetupFunc setupFunc;
    RunFunc runFunc;
    std::vector<std::reference_wrapper<Command>> subcommands;
public:
    Command(const std::string &name, const std::optional<std::string>& version,
            SetupFunc _setupFunc, RunFunc _runFunc,
            const std::vector<std::reference_wrapper<Command>>& _subcommands = {})
        : argparse::ArgumentParser(name, version? version.value() : "0.0", 
            version? argparse::default_arguments::all : argparse::default_arguments::help), 
        setupFunc(_setupFunc), runFunc(_runFunc), subcommands(_subcommands) {
    }

    void setup() {
        for (const auto &subcommand : subcommands) {
            subcommand.get().setup();
            add_subparser(subcommand);
        }
        setupFunc(*this);
    }
    int run() {
        for (const auto &subcommand : subcommands) {
            if (is_subcommand_used(subcommand))
                return subcommand.get().run();
        }
        return runFunc(*this);
    }

    std::optional<std::reference_wrapper<Command>> get_used_subcommand() const {
        for (const auto &subcommand : subcommands) {
            if (is_subcommand_used(subcommand))
                return subcommand;
        }
        return std::nullopt;
    }

    void print_usage(std::ostream& out) const {
        auto subcommand = get_used_subcommand();
        if (subcommand) {
            subcommand->get().print_usage(out);
        } else {
            out << *this;
        }
    }
};

namespace subcommand {

    static Command start("start", std::nullopt, 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Start VM");
            parser.add_argument("-c", "--console").default_value(false).implicit_value(true);
            parser.add_argument("vmname").nargs(1);
        },
        [](const argparse::ArgumentParser& parser) {
            return vm::start(parser.get("vmname"), parser.get<bool>("-c"));
        }
    );

    static Command stop("stop", std::nullopt, 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Stop VM");
            parser.add_argument("-c", "--console").default_value(false).implicit_value(true);
            parser.add_argument("-f", "--force").default_value(false).implicit_value(true);
            parser.add_argument("vmname").nargs(1);
        },
        [](const argparse::ArgumentParser& parser) {
            return vm::stop(parser.get("vmname"), parser.get<bool>("-f"), parser.get<bool>("-c"));
        }
    );

    static Command restart("restart", std::nullopt, 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Restart VM");
            parser.add_argument("-f", "--force").default_value(false).implicit_value(true);
            parser.add_argument("vmname").nargs(1);
        },
        [](const argparse::ArgumentParser& parser) {
            return vm::restart(parser.get("vmname"), parser.get<bool>("-f"));
        }
    );

    static Command console("console", std::nullopt,
        [](auto& parser) {
            parser.add_description("Connect to VM console");
            parser.add_argument("vmname").nargs(1);            
        },
        [](const auto& parser) {
            return vm::console(parser.get("vmname"));
        }
    );

    static Command autostart("autostart", std::nullopt,
        [](auto& parser) {
            parser.add_description("Enable/disable VM autostart");
            parser.add_argument("vmname").nargs(1);
            parser.add_argument("onoff").nargs(argparse::nargs_pattern::optional);
        },
        [](const auto& parser) {
            std::optional<bool> onoff = std::nullopt;
            if (parser.is_used("onoff")) {
                auto onoff_str = parser.get("onoff");
                if (onoff_str == "on") onoff = true;
                else if (onoff_str == "off") onoff = false;
                else throw std::runtime_error("'on' or 'off' must be specified");
            }
            return vm::autostart(parser.get("vmname"), onoff);
        }
    );

    static Command list("list", std::nullopt, 
        [](auto& parser) {
            parser.add_description("List VMs");
        },
        [](const auto& parser) {
            return vm::list(vm_root());
        }
    );

    static Command create("create", std::nullopt,
        [](auto& parser) {
            parser.add_description("Create new VM");
            if (is_root_user()) 
                parser.add_argument("--volume", "-v").template default_value<std::string>("default").help("Specify volume to create VM on");
            parser.add_argument("--memory", "-m").template scan<'u',uint32_t>().help("Memory capacity in MB");
            parser.add_argument("--cpu", "-c").template scan<'u',uint16_t>().help("Number of CPU");
            parser.add_argument("--data-partition")
                .nargs(1)
                .template scan<'u',uint32_t>()
                .help("Create uninitialized data partition with specified size in GiB");
            parser.add_argument("vmname").nargs(1).help("VM name");
            parser.add_argument("system-file").nargs(argparse::nargs_pattern::optional);
        },
        [](const auto& parser) {
            return vm::create(vm_root(), parser.get("vmname"), {
                .volume = is_root_user()? std::make_optional(parser.get("--volume")) : std::nullopt,
                .memory = parser.template present<uint32_t>("--memory"),
                .cpu = parser.template present<uint16_t>("--cpu"),
                .data_partition = parser.template present<uint32_t>("--data-partition"),
                .system_file = parser.present("system-file")
            });
        }
    );

    static Command _delete("delete", std::nullopt,
        [](auto& parser) {
            parser.add_description("Delete VM");
            parser.add_argument("vmname").help("VM name");
        },
        [](const auto& parser) {
            return vm::_delete(vm_root(), parser.get("vmname"));
        }
    );

    static Command install("install", std::nullopt,
        [](auto& parser) {
            parser.add_argument("-i", "--system-image").nargs(1).template default_value<std::string>("/run/initramfs/boot/system.img");
            parser.add_argument("--text-mode").default_value(false).implicit_value(true);
            parser.add_argument("--installer").default_value(false).implicit_value(true);
            parser.add_argument("disk").nargs(argparse::nargs_pattern::optional);
        },
        [](const auto& parser) {
            must_be_root();
            if (parser.is_used("disk")) {
                return install::install(parser.get("disk"), parser.get("-i"), 
                    parser.template get<bool>("--text-mode"), parser.template get<bool>("--installer"));
            }
            //else
            std::cout << parser;
            std::cout << std::endl;
            std::cout << "Usable disks below:" << std::endl;
            return install::show_usable_disks();
        }
    );

    static Command invoke("invoke", std::nullopt,
        [](auto& parser) {
        },
        [](const auto& parser) {
            return invoke::invoke();
        }
    );

    // volume subcommands
    static Command volume_add("add", std::nullopt, 
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
            parser.add_argument("device").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::add(vm_root(), parser.get("name"), parser.get("device"));
        }
    );

    static Command volume_remove("remove", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::remove(vm_root(), parser.get("name"));
        }
    );

    static Command volume_scan("scan", std::nullopt,
        [](argparse::ArgumentParser& parser) {
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::scan(vm_root());
        }
    );

    static Command volume_list("list", std::nullopt, 
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("-n", "--names-only").default_value(false).implicit_value(true);
            parser.add_argument("-o", "--online-only").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::list(vm_root(), {
                .online_only = parser.get<bool>("-o"),
                .names_only = parser.get<bool>("-n")
            });
        }
    );

    static Command volume_snapshot("snapshot", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::snapshot(vm_root(), parser.get("name"));
        }
    );

    static Command volume_backup("backup", std::nullopt, 
        [](argparse::ArgumentParser& parser) {
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::backup(vm_root());
        }
    );

    static Command volume_clean("clean", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(argparse::nargs_pattern::optional);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::clean(vm_root(), parser.present("name"));
        }
    );

    static Command volume_optimize("optimize", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::optimize(vm_root(), parser.get("name"));
        }
    );

    static Command volume("volume", std::nullopt, 
        [](argparse::ArgumentParser&){}, 
        [](const argparse::ArgumentParser& parser){
            std::cerr << "No subcommand specified for volume" << std::endl;
            std::cout << parser << std::endl;
            return -1;
        },
        {
            volume_add, volume_remove, volume_scan, volume_list, volume_snapshot,
            volume_backup, volume_clean, volume_optimize
        }
    );

    // wg subcommands
    static Command wg_genkey("genkey", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Generate WireGuard key");
            parser.add_argument("-f", "--force").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            return wg::genkey(parser.get<bool>("--force"));
        }
    );

    static Command wg_pubkey("pubkey", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Show WireGuard public key");
            parser.add_argument("-q", "--qrcode").help("Show QR code instead of text").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            return wg::pubkey(parser.get<bool>("--qrcode"));
        }
    );

    static Command wg_getconfig("getconfig", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Get authorized WireGuard config from server");
            parser.add_argument("--accept-ssh-key", "-k").help("Accept SSH public key").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            return wg::getconfig(parser.get<bool>("--accept-ssh-key"));
        }
    );

    static Command wg_notify("notify", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Send notification message via HTTP over WireGuard");
            parser.add_argument("uri").nargs(1).help("URI to get");
        },[](const argparse::ArgumentParser& parser) {
            return wg::notify(parser.get("uri"));
        }
    );

    static Command wg_ping("ping", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Ping WireGuard peer");
            parser.add_argument("-q", "--quiet").default_value(false).implicit_value(true);
            parser.add_argument("--success-if-not-active").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            return wg::ping(parser.get<bool>("--success-if-not-active"), 5, !parser.get<bool>("--quiet"));
        }
    );

    static Command wg("wg", std::nullopt, 
        [](argparse::ArgumentParser&){}, 
        [](const argparse::ArgumentParser& parser){
            std::cerr << "No subcommand specified for wg" << std::endl;
            std::cout << parser << std::endl;
            return -1;
        },
        {
            wg_genkey, wg_pubkey, wg_getconfig, wg_notify, wg_ping
        }
    );

    // misc subcommands
    static Command misc_wayland_ping("wayland-ping", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Ping Wayland compositor");
            parser.add_argument("-q", "--quiet").default_value(false).implicit_value(true);
            parser.add_argument("-w", "--wait").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            auto rst = wayland_ping(parser.get<bool>("--wait"));
            if (!parser.get<bool>("--quiet")) {
                if (rst) std::cout << "Wayland display is alive." << std::endl;
                else std::cout << "Wayland display is not available." << std::endl;
            }
            return rst? 0 : 1;
        }
    );

    static Command misc_generate_rdp_cert("generate-rdp-cert", std::nullopt, 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Generate certificate for RDP");
        },[](const argparse::ArgumentParser& parser) {
            return generate_rdp_cert();
        }
    );

    static Command misc_list_wwid("list-wwid", std::nullopt,
        [](argparse::ArgumentParser& parser) {
            parser.add_description("List WWID of all block devices");
        },[](const argparse::ArgumentParser& parser) {
            list_wwid();
            return 0;
        }
    );

    static Command misc("misc", std::nullopt, 
        [](argparse::ArgumentParser&){}, 
        [](const argparse::ArgumentParser& parser){
            std::cerr << "No subcommand specified for misc" << std::endl;
            std::cout << parser << std::endl;
            return -1;
        },
        {
            misc_wayland_ping, misc_generate_rdp_cert, misc_list_wwid
        }
    );
} // namespace subcommand

static int _main(int argc, char* argv[])
{
    Command program(argv[0], VERSION, [](argparse::ArgumentParser& parser){

    }, [&program](const argparse::ArgumentParser& parser){
        auto subcommand = program.get_used_subcommand();
        if (subcommand) {
            return subcommand->get().run();
        }
        //else
        std::cerr << "No subcommand specified" << std::endl;
        std::cout << program;
        return -1;
    }, {
        subcommand::start, subcommand::stop, subcommand::restart, subcommand::console, subcommand::autostart,
        subcommand::list, subcommand::create, subcommand::_delete, subcommand::install, subcommand::invoke,
        subcommand::volume, subcommand::wg, subcommand::misc
    });

    program.setup();

    try {
        program.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        program.print_usage(std::cout);
        return -1;
    }

    return program.run();
}

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
    char* _argv[] = {
       argv[0],
        "create",
        "--help"
    };
    int _argc = sizeof(_argv) / sizeof(_argv[0]);
    return _main(_argc, _argv);
}
#endif

#ifdef __USE_REAL_MAIN__
int main(int argc, char* argv[])
{
    try {
        return _main(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        return 1;
    }
}
#endif
