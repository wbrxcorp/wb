#include <pwd.h>
#include <unistd.h>

#include <argparse/argparse.hpp>

#include "vm.h"
#include "volume.h"
#include "install.h"
#include "wg.h"
#include "misc.h"
#include "invoke.h"

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

typedef std::tuple<
    argparse::ArgumentParser,
    std::function<void(argparse::ArgumentParser&)>,
    std::function<int(const argparse::ArgumentParser&)>
> SubSubCommand;

typedef std::pair<
    argparse::ArgumentParser,
    std::variant<
        std::pair<
            std::function<void(argparse::ArgumentParser&)>,
            std::function<int(const argparse::ArgumentParser&)>
        >,
        std::map<std::string,SubSubCommand>
    >
> SubCommand;

namespace subcommand {

    static SubCommand start = {
        argparse::ArgumentParser("start"), std::make_pair(
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Start VM");
            parser.add_argument("-c", "--console").default_value(false).implicit_value(true);
            parser.add_argument("vmname").nargs(1);
        },
        [](const argparse::ArgumentParser& parser) {
            return vm::start(parser.get("vmname"), parser.get<bool>("-c"));
        }
    )};

    static SubCommand stop = {
        argparse::ArgumentParser("stop"), std::make_pair(
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Stop VM");
            parser.add_argument("-c", "--console").default_value(false).implicit_value(true);
            parser.add_argument("-f", "--force").default_value(false).implicit_value(true);
            parser.add_argument("vmname").nargs(1);
        },
        [](const argparse::ArgumentParser& parser) {
            return vm::stop(parser.get("vmname"), parser.get<bool>("-f"), parser.get<bool>("-c"));
        }
    )};

    static SubCommand restart = {
        argparse::ArgumentParser("restart"), std::make_pair(
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Restart VM");
            parser.add_argument("-f", "--force").default_value(false).implicit_value(true);
            parser.add_argument("vmname").nargs(1);
        },
        [](const argparse::ArgumentParser& parser) {
            return vm::restart(parser.get("vmname"), parser.get<bool>("-f"));
        }
    )};

    static SubCommand console = {
        argparse::ArgumentParser("console"), std::make_pair(
            [](auto& parser) {
                parser.add_description("Connect to VM console");
                parser.add_argument("vmname").nargs(1);            
            },
            [](const auto& parser) {
                return vm::console(parser.get("vmname"));
            }
        )
    };

    static SubCommand autostart = {
        argparse::ArgumentParser("autostart"), std::make_pair(
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
        )
    };

    static SubCommand list = {
        argparse::ArgumentParser("list"), std::make_pair(
            [](auto& parser) {
                parser.add_description("List VMs");
            },
            [](const auto& parser) {
                return vm::list(vm_root());
            }
        )
    };

    static SubCommand create = {
        argparse::ArgumentParser("create"), std::make_pair(
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
        )
    };

    static SubCommand _delete = {
        argparse::ArgumentParser("delete"), std::make_pair(
            [](auto& parser) {
                parser.add_description("Delete VM");
                parser.add_argument("vmname").help("VM name");
            },
            [](const auto& parser) {
                return vm::_delete(vm_root(), parser.get("vmname"));
            }
        )
    };

    static SubCommand install = {
        argparse::ArgumentParser("install"), std::make_pair(
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
        )
    };

    static SubCommand invoke = {
        argparse::ArgumentParser("invoke"), std::make_pair(
            [](auto& parser) {
            },
            [](const auto& parser) {
                return invoke::invoke();
            }
        )
    };

    // volume subsubcommands
    static SubSubCommand volume_add = {
        argparse::ArgumentParser("add"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
            parser.add_argument("device").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::add(vm_root(), parser.get("name"), parser.get("device"));
        }
    };

    static SubSubCommand volume_remove = {
        argparse::ArgumentParser("remove"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::remove(vm_root(), parser.get("name"));
        }
    };

    static SubSubCommand volume_scan = {
        argparse::ArgumentParser("scan"), 
        [](argparse::ArgumentParser& parser) {
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::scan(vm_root());
        }
    };

    static SubSubCommand volume_list = {
        argparse::ArgumentParser("list"), 
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
    };

    static SubSubCommand volume_snapshot = {
        argparse::ArgumentParser("snapshot"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::snapshot(vm_root(), parser.get("name"));
        }
    };

    static SubSubCommand volume_backup = {
        argparse::ArgumentParser("backup"), 
        [](argparse::ArgumentParser& parser) {
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::backup(vm_root());
        }
    };

    static SubSubCommand volume_clean = {
        argparse::ArgumentParser("clean"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(argparse::nargs_pattern::optional);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::clean(vm_root(), parser.present("name"));
        }
    };

    static SubSubCommand volume_optimize = {
        argparse::ArgumentParser("optimize"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_argument("name").nargs(1);
        },[](const argparse::ArgumentParser& parser) {
            must_be_root();
            return volume::optimize(vm_root(), parser.get("name"));
        }
    };

    // wg subsubcommands

    static SubSubCommand wg_genkey = {
        argparse::ArgumentParser("genkey"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Generate WireGuard key");
            parser.add_argument("-f", "--force").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            return wg::genkey(parser.get<bool>("--force"));
        }
    };

    static SubSubCommand wg_pubkey = {
        argparse::ArgumentParser("pubkey"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Show WireGuard public key");
            parser.add_argument("-q", "--qrcode").help("Show QR code instead of text").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            return wg::pubkey(parser.get<bool>("--qrcode"));
        }
    };

    static SubSubCommand wg_getconfig = {
        argparse::ArgumentParser("getconfig"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Get authorized WireGuard config from server");
            parser.add_argument("--accept-ssh-key", "-k").help("Accept SSH public key").default_value(false).implicit_value(true);
        },[](const argparse::ArgumentParser& parser) {
            return wg::getconfig(parser.get<bool>("--accept-ssh-key"));
        }
    };

    static SubSubCommand wg_notify = {
        argparse::ArgumentParser("notify"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Send notification message via HTTP over WireGuard");
            parser.add_argument("uri").nargs(1).help("URI to get");
        },[](const argparse::ArgumentParser& parser) {
            return wg::notify(parser.get("uri"));
        }
    };

    static SubSubCommand misc_wayland_ping = {
        argparse::ArgumentParser("wayland-ping"), 
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
    };

    static SubSubCommand misc_generate_rdp_cert = {
        argparse::ArgumentParser("generate-rdp-cert"), 
        [](argparse::ArgumentParser& parser) {
            parser.add_description("Generate certificate for RDP");
        },[](const argparse::ArgumentParser& parser) {
            return generate_rdp_cert();
        }
    };

} // namespace subcommand

static std::map<std::string,SubCommand> subcommands = {
    {"start", subcommand::start},
    {"stop", subcommand::stop},
    {"restart", subcommand::restart},
    {"console", subcommand::console},
    {"autostart", subcommand::autostart},
    {"list", subcommand::list},
    {"create", subcommand::create},
    {"delete", subcommand::_delete},
    {"install", subcommand::install},
    {"invoke", subcommand::invoke},
    {"volume", {argparse::ArgumentParser("volume"), std::map<std::string,SubSubCommand> {
        {"add", subcommand::volume_add},
        {"remove", subcommand::volume_remove},
        {"scan", subcommand::volume_scan},
        {"list", subcommand::volume_list},
        {"snapshot", subcommand::volume_snapshot},
        {"backup", subcommand::volume_backup},
        {"clean", subcommand::volume_clean},
        {"optimize", subcommand::volume_optimize}
    }}},
    {"wg", {argparse::ArgumentParser("wg"), std::map<std::string,SubSubCommand> {
        {"genkey", subcommand::wg_genkey},
        {"pubkey", subcommand::wg_pubkey},
        {"getconfig", subcommand::wg_getconfig},
        {"notify", subcommand::wg_notify},
    }}},
    {"misc", {argparse::ArgumentParser("misc"), std::map<std::string,SubSubCommand> {
        {"wayland-ping", subcommand::misc_wayland_ping},
        {"generate-rdp-cert", subcommand::misc_generate_rdp_cert},
    }}}
};

static int _main(int argc, char* argv[])
{
    argparse::ArgumentParser program(argv[0]);

    for (auto& [name, subcommand] : subcommands) {
        auto& [parser, func_or_subsubcommand] = subcommand;
        if (auto* p = std::get_if<std::pair<std::function<void(argparse::ArgumentParser&)>,std::function<int(const argparse::ArgumentParser&)>>>(&func_or_subsubcommand)) {
            auto& [func1, func2] = *p;
            func1(parser);
        } else if (auto* p = std::get_if<std::map<std::string,SubSubCommand>>(&func_or_subsubcommand)) {
            auto& subsubcommands = *p;
            for (auto& [name, subsubcommand] : subsubcommands) {
                auto& [subparser, func1, func2] = subsubcommand;
                func1(subparser);
                parser.add_subparser(subparser);
            }
        }
        program.add_subparser(parser);
    }

    try {
        program.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        bool help_printed = false;
        for (auto& [name, subcommand] : subcommands) {
            if (program.is_subcommand_used(name)) {
                auto& [parser, func_or_subsubcommand] = subcommand;
                if (auto* p = std::get_if<std::map<std::string,SubSubCommand>>(&func_or_subsubcommand)) {
                    auto& subsubcommands = *p;
                    for (auto& [name, subsubcommand] : subsubcommands) {
                        if (parser.is_subcommand_used(name)) {
                            auto& [subparser, func1, func2] = subsubcommand;
                            std::cerr << subparser;
                            help_printed = true;
                            break;
                        }
                    }
                }
                if (!help_printed) {
                    std::cerr << parser;
                    help_printed = true;
                    break;
                }
            }
        }
        if (!help_printed) {
            std::cerr << program;
        }
        return 1;
    }

    for (auto& [name, subcommand] : subcommands) {
        if (!program.is_subcommand_used(name)) continue;
        auto& [parser, func_or_subsubcommand] = subcommand;
        if (auto* p = std::get_if<std::pair<std::function<void(argparse::ArgumentParser&)>,std::function<int(const argparse::ArgumentParser&)>>>(&func_or_subsubcommand)) {
            auto& [func1, func2] = *p;
            return func2(parser);
        } else if (auto* p = std::get_if<std::map<std::string,SubSubCommand>>(&func_or_subsubcommand)) {
            auto& subsubcommands = *p;
            for (auto& [name, subsubcommand] : subsubcommands) {
                if (!parser.is_subcommand_used(name)) continue;
                auto& [subparser, func1, func2] = subsubcommand;
                return func2(subparser);
            }
            std::cout << parser;
            return 1;
        }
    }

    std::cout << program;
    return 1;
}

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
    return _main(argc, argv);
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
