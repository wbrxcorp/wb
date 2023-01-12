#include <pwd.h>
#include <unistd.h>

#include <argparse/argparse.hpp>

#include "vm.h"
#include "volume.h"
#include "install.h"
#include "wg.h"

static bool is_root_user()
{
    return (getuid() == 0);
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

static int _main(int argc, char* argv[])
{
    argparse::ArgumentParser program(argv[0]);

    // VM subcommands
    argparse::ArgumentParser start_command("start");
    start_command.add_description("Start VM");
    start_command.add_argument("-c", "--console").default_value(false).implicit_value(true);
    start_command.add_argument("vmname").nargs(1);
    program.add_subparser(start_command);

    argparse::ArgumentParser stop_command("stop");
    stop_command.add_description("Stop VM");
    stop_command.add_argument("-c", "--console").default_value(false).implicit_value(true);
    stop_command.add_argument("-f", "--force").default_value(false).implicit_value(true);
    stop_command.add_argument("vmname").nargs(1);
    program.add_subparser(stop_command);

    argparse::ArgumentParser restart_command("restart");
    restart_command.add_description("Restart VM");
    restart_command.add_argument("-f", "--force").default_value(false).implicit_value(true);
    restart_command.add_argument("vmname").nargs(1);
    program.add_subparser(restart_command);

    argparse::ArgumentParser console_command("console");
    console_command.add_description("Connect to VM console");
    console_command.add_argument("vmname").nargs(1);
    program.add_subparser(console_command);

    argparse::ArgumentParser autostart_command("autostart");
    autostart_command.add_argument("vmname").nargs(1);
    autostart_command.add_argument("onoff").nargs(argparse::nargs_pattern::optional);
    program.add_subparser(autostart_command);

    argparse::ArgumentParser list_command("list");
    program.add_subparser(list_command);

    argparse::ArgumentParser create_command("create");
    if (is_root_user()) 
        create_command.add_argument("--volume", "-v").default_value<std::string>("default").help("Specify volume to create VM on");
    create_command.add_argument("--memory", "-m").scan<'u',uint32_t>().help("Memory capacity in MB");
    create_command.add_argument("--cpu", "-c").scan<'u',uint16_t>().help("Number of CPU");
    create_command.add_argument("--data-partition")
        .nargs(1)
        .scan<'u',uint16_t>()
        .help("Create uninitialized data partition with specified size in GiB");
    create_command.add_argument("vmname").nargs(1).help("VM name");
    create_command.add_argument("system-file").nargs(argparse::nargs_pattern::optional);
    program.add_subparser(create_command);

    argparse::ArgumentParser delete_command("delete");
    delete_command.add_argument("vmname").help("VM name");
    program.add_subparser(delete_command);

    // Volume subcommands
    argparse::ArgumentParser volume_command("volume");
    argparse::ArgumentParser volume_add_command("add");
    volume_add_command.add_argument("name").nargs(1);
    volume_add_command.add_argument("device").nargs(1);
    volume_command.add_subparser(volume_add_command);
    argparse::ArgumentParser volume_remove_command("remove");
    volume_remove_command.add_argument("name").nargs(1);
    volume_command.add_subparser(volume_remove_command);
    argparse::ArgumentParser volume_scan_command("scan");
    volume_command.add_subparser(volume_scan_command);
    argparse::ArgumentParser volume_list_command("list");
    volume_command.add_subparser(volume_list_command);
    argparse::ArgumentParser volume_backup_command("backup");
    volume_command.add_subparser(volume_backup_command);
    argparse::ArgumentParser volume_clean_command("clean");
    volume_clean_command.add_description("Cleanup .trash directory");
    volume_clean_command.add_argument("volume").nargs(argparse::nargs_pattern::optional).help("Volume to clean");
    volume_command.add_subparser(volume_clean_command);
    program.add_subparser(volume_command);

    argparse::ArgumentParser install_command("install");
    install_command.add_argument("-i", "--system-image").nargs(1).default_value<std::string>("/run/initramfs/boot/system.img");
    install_command.add_argument("--text-mode").default_value(false).implicit_value(true);
    install_command.add_argument("--installer").default_value(false).implicit_value(true);
    install_command.add_argument("disk").nargs(argparse::nargs_pattern::optional);
    program.add_subparser(install_command);

    // WireGuard subcommands
    argparse::ArgumentParser wg_command("wg");
    wg_command.add_description("WireGuard related commmands");
    argparse::ArgumentParser wg_genkey_command("genkey");
    wg_genkey_command.add_argument("-f", "--force").default_value(false).implicit_value(true);
    wg_genkey_command.add_description("Generate WireGuard key");
    wg_command.add_subparser(wg_genkey_command);
    argparse::ArgumentParser wg_pubkey_command("pubkey");
    wg_pubkey_command.add_description("Show WireGuard public key");
    wg_pubkey_command.add_argument("-q", "--qrcode").help("Show QR code instead of text").default_value(false).implicit_value(true);
    wg_command.add_subparser(wg_pubkey_command);
    argparse::ArgumentParser wg_getconfig_command("getconfig");
    wg_getconfig_command.add_description("Get authorized WireGuard config from server");
    wg_getconfig_command.add_argument("--accept-ssh-key", "-k").help("Accept SSH public key").default_value(false).implicit_value(true);
    wg_command.add_subparser(wg_getconfig_command);
    argparse::ArgumentParser wg_notify_command("notify");
    wg_notify_command.add_description("Send notification message via HTTP over WireGuard");
    wg_notify_command.add_argument("uri").nargs(1).help("URI to get");
    wg_command.add_subparser(wg_notify_command);
    program.add_subparser(wg_command);

    try {
        program.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        if (program.is_subcommand_used("start")) {
            std::cerr << start_command;
        } else if (program.is_subcommand_used("stop")) {
            std::cerr << stop_command;
        } else if (program.is_subcommand_used("restart")) {
            std::cerr << restart_command;
        } else if (program.is_subcommand_used("console")) {
            std::cerr << console_command;
        } else if (program.is_subcommand_used("autostart")) {
            std::cerr << autostart_command;
        } else if (program.is_subcommand_used("list")) {
            std::cerr << list_command;
        } else if (program.is_subcommand_used("create")) {
            std::cerr << create_command;
        } else if (program.is_subcommand_used("delete")) {
            std::cerr << delete_command;
        } else if (program.is_subcommand_used("volume")) {
            if (volume_command.is_subcommand_used("add")) {
                std::cerr << volume_add_command;
            } else if (volume_command.is_subcommand_used("remove")) {
                std::cerr << volume_remove_command;
            } else if (volume_command.is_subcommand_used("scan")) {
                std::cerr << volume_scan_command;
            } else if (volume_command.is_subcommand_used("list")) {
                std::cerr << volume_list_command;
            } else if (volume_command.is_subcommand_used("backup")) {
                std::cerr << volume_backup_command;
            } else if (volume_command.is_subcommand_used("clean")) {
                std::cerr << volume_clean_command;
            }
        } else if (program.is_subcommand_used("install")) {
            std::cerr << install_command;
        } else if (program.is_subcommand_used("wg")) {
            if (wg_command.is_subcommand_used("genkey")) {
                std::cerr << wg_genkey_command;
            } else if (wg_command.is_subcommand_used("pubkey")) {
                std::cerr << wg_pubkey_command;
            } else if (wg_command.is_subcommand_used("getconfig")) {
                std::cerr << wg_getconfig_command;
            } else if (wg_command.is_subcommand_used("notify")) {
                std::cerr << wg_notify_command;
            }
        } else {
            std::cerr << program;
        }
        return 1;
    }
    catch (const std::logic_error& err) {
        std::cerr << err.what() << std::endl;
        return 1;
    }

    if (program.is_subcommand_used("start")) {
        return vm::start(start_command.get("vmname"), start_command.get<bool>("-c"));
    }
    if (program.is_subcommand_used("stop")) {
        return vm::stop(stop_command.get("vmname"), stop_command.get<bool>("-f"), stop_command.get<bool>("-c"));
    }
    if (program.is_subcommand_used("restart")) {
        return vm::restart(restart_command.get("vmname"), restart_command.get<bool>("-f"));
    }
    if (program.is_subcommand_used("console")) {
        return vm::console(console_command.get("vmname"));
    }
    if (program.is_subcommand_used("autostart")) {
        std::optional<bool> onoff = std::nullopt;
        if (autostart_command.is_used("onoff")) {
            auto onoff_str = autostart_command.get("onoff");
            if (onoff_str == "on") onoff = true;
            else if (onoff_str == "off") onoff = false;
            else throw std::runtime_error("'on' or 'off' must be specified");
        }
        return vm::autostart(autostart_command.get("vmname"), onoff);
    }
    if (program.is_subcommand_used("list")) {
        return vm::list(vm_root());
    }
    if (program.is_subcommand_used("create")) {
        return vm::create(vm_root(), create_command.get("vmname"), {
            .volume = is_root_user()? std::make_optional(create_command.get("--volume")) : std::nullopt,
            .memory = create_command.present<uint32_t>("--memory"),
            .cpu = create_command.present<uint16_t>("--cpu"),
            .data_partition = create_command.present<uint16_t>("--data-partition"),
            .system_file = create_command.present("system-file")
        });
    }
    if (program.is_subcommand_used("delete")) {
        return vm::_delete(vm_root(), delete_command.get("vmname"));
    }
    if (program.is_subcommand_used("volume")) {
        if (volume_command.is_subcommand_used("add")) {
            return volume::add(vm_root(), volume_add_command.get("name"), volume_add_command.get("device"));
        }
        if (volume_command.is_subcommand_used("remove")) {
            return volume::remove(vm_root(), volume_remove_command.get("name"));
        }
        if (volume_command.is_subcommand_used("scan")) {
            return volume::scan(vm_root());
        }
        if (volume_command.is_subcommand_used("list")) {
            return volume::list(vm_root());
        }
        if (volume_command.is_subcommand_used("backup")) {
            return volume::backup(vm_root());
        }
        if (volume_command.is_subcommand_used("clean")) {
            return volume::clean(vm_root(), volume_clean_command.present("volume"));
        }
        std::cout << volume_command;
        return 1;
    }
    if (program.is_subcommand_used("install")) {
        if (install_command.is_used("disk")) {
            return install::install(install_command.get("disk"), install_command.get("-i"), 
                install_command.get<bool>("--text-mode"), install_command.get<bool>("--installer"));
        }
        //else
        std::cout << install_command;
        std::cout << std::endl;
        std::cout << "Usable disks below:" << std::endl;
        return install::show_usable_disks();
    }
    if (program.is_subcommand_used("wg")) {
        if (wg_command.is_subcommand_used("genkey")) {
            return wg::genkey(wg_genkey_command.get<bool>("--force"));
        }
        if (wg_command.is_subcommand_used("pubkey")) {
            return wg::pubkey(wg_pubkey_command.get<bool>("--qrcode"));
        }
        if (wg_command.is_subcommand_used("getconfig")) {
            return wg::getconfig(wg_getconfig_command.get<bool>("--accept-ssh-key"));
        }
        if (wg_command.is_subcommand_used("notify")) {
            return wg::notify(wg_notify_command.get("uri"));
        }
        //else
        std::cout << wg_command;
        return 1;
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
