#include <unistd.h>
#include <glob.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <iostream>

#include <libmount/libmount.h>
#include <blkid/blkid.h>
#include <libsmartcols/libsmartcols.h>

#include <ext/stdio_filebuf.h> // for __gnu_cxx::stdio_filebuf
#include <nlohmann/json.hpp>

#include "misc.h"
#include "install.h"

static void exec_command(const std::string& cmd, const std::vector<std::string>& args)
{
    pid_t pid = fork();
    if (pid < 0) std::runtime_error("fork() failed");
    //else
    if (pid == 0) { //child
        char* argv[args.size() + 2];
        int i = 0;
        argv[i++] = strdup(cmd.c_str());
        for (auto arg : args) {
            argv[i++] = strdup(arg.c_str());
        }
        argv[i] = NULL;
        _exit(execvp(cmd.c_str(), argv));
    }
    // else {
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)  != 0) throw std::runtime_error(cmd);
}

static int glob(const char* pattern, int flags, int errfunc(const char *epath, int eerrno), std::vector<std::filesystem::path>& match)
{
  glob_t globbuf;
  match.clear();
  int rst = glob(pattern, GLOB_NOESCAPE, NULL, &globbuf);
  if (rst == GLOB_NOMATCH) return 0;
  if (rst != 0) throw std::runtime_error("glob");
  //else
  for (int i = 0; i < globbuf.gl_pathc; i++) {
    match.push_back(std::filesystem::path(globbuf.gl_pathv[i]));
  }
  globfree(&globbuf);
  return match.size();
}

static std::optional<std::filesystem::path> get_partition(const std::filesystem::path& disk, uint8_t num)
{
  if (!std::filesystem::is_block_file(disk)) throw std::runtime_error("Not a block device");

  struct stat s;
  if (stat(disk.c_str(), &s) < 0) throw std::runtime_error("stat");

  char pattern[128];
  sprintf(pattern, "/sys/dev/block/%d:%d/*/partition",
    major(s.st_rdev), minor(s.st_rdev));

  std::vector<std::filesystem::path> match;
  glob(pattern, GLOB_NOESCAPE, NULL, match);
  for (auto& path: match) {
    std::ifstream part(path);
    uint16_t partno;
    part >> partno;
    if (partno == num) {
      std::ifstream dev(path.replace_filename("dev"));
      std::string devno;
      dev >> devno;
      std::filesystem::path devblock("/dev/block/");
      auto devspecial = std::filesystem::read_symlink(devblock.replace_filename(devno));
      return devspecial.is_absolute()? devspecial : std::filesystem::canonical(devblock.replace_filename(devspecial));
    }
  }
  return std::nullopt;
}

static std::optional<std::string> get_partition_uuid(const std::filesystem::path& partition)
{
  blkid_cache cache;
  if (blkid_get_cache(&cache, "/dev/null") != 0) return std::nullopt;
  // else
  std::optional<std::string> rst = std::nullopt;
  if (blkid_probe_all(cache) == 0) {
    auto tag_value = blkid_get_tag_value(cache, "UUID", partition.c_str());
    if (tag_value) rst = tag_value;
  }
  blkid_put_cache(cache);
  return rst;
}

static bool is_all_descendants_free(nlohmann::json& blockdevice)
{
    if (!blockdevice.contains("children")) return true;
    //else
    for (auto& child : blockdevice["children"]) {
        if (!child["mountpoint"].is_null() || child["type"] != "part") return false;
        //else
        if (!is_all_descendants_free(child)) return false;
    }
    return true;
}

static nlohmann::json lsblk()
{
    auto fd = memfd_create("lsblk", 0);
    if (fd < 0) throw std::runtime_error("memfd_create() failed");

    __gnu_cxx::stdio_filebuf<char> filebuf(fd, std::ios::in); // this cleans up fd in dtor

    auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        dup2(fd, STDOUT_FILENO);
        _exit(execlp("lsblk", "lsblk", "-b", "-n", "-J", "-o", "NAME,MODEL,TYPE,RO,MOUNTPOINT,SIZE,TRAN,LOG-SEC,MAJ:MIN", NULL));
    }
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        throw std::runtime_error("subprocess exited with error");
    }
    lseek(fd, 0, SEEK_SET);
    std::istream f(&filebuf);
    return nlohmann::json::parse(f)["blockdevices"];
}

struct Disk {
    std::string name;
    std::optional<std::string> model;
    uint64_t size;
    std::optional<std::string> tran;
    uint16_t log_sec;
};

static std::map<std::string,Disk> enum_usable_disks(uint64_t least_size)
{
    std::map<std::string,Disk> disks;
    for (auto& blockdevice : lsblk()) {
        if (blockdevice["type"] != "disk" || blockdevice["ro"] == true /*|| blockdevice["tran"].is_null() // virtio disks doesn't have this*/
            || blockdevice["size"].get<uint64_t>() < least_size || !blockdevice["log-sec"].is_number_integer()
            || !is_all_descendants_free(blockdevice)) continue;
        std::string name = blockdevice["name"];
        disks["/dev/" + name] = {
            .name = name,
            .model = blockdevice["model"].is_null()? std::nullopt : std::make_optional(blockdevice["model"]),
            .size = blockdevice["size"].get<uint64_t>(),
            .tran = blockdevice["tran"].is_null()? std::nullopt : std::make_optional(blockdevice["tran"]),
            .log_sec = blockdevice["log-sec"].get<uint16_t>()
        };
    }
    return disks;
}

template <typename T> T with_tempmount(const std::filesystem::path& device, const char* fstype, int flags, const char* data,
    std::function<T(const std::filesystem::path&)> func)
{
    struct libmnt_context *ctx = mnt_new_context();
    if (!ctx) throw std::runtime_error("mnt_new_context");
    // else

    auto path = std::filesystem::temp_directory_path() /= std::string("mount-") + std::to_string(getpid());
    std::filesystem::create_directory(path);
    mnt_context_set_fstype_pattern(ctx, fstype);
    mnt_context_set_source(ctx, device.c_str());
    mnt_context_set_target(ctx, path.c_str());
    mnt_context_set_mflags(ctx, flags);
    mnt_context_set_options(ctx, data);
    auto rst = mnt_context_mount(ctx);
    auto status1 = mnt_context_get_status(ctx);
    auto status2 = mnt_context_get_helper_status(ctx);
    mnt_free_context(ctx);
    if (rst > 1) throw std::runtime_error("mnt_context_mount");
    if (rst != 0) throw std::runtime_error("mnt_context_mount");
    //else
    if (status1 != 1) throw std::runtime_error("mnt_context_get_status");
    if (status2 != 0) throw std::runtime_error("mnt_context_get_helper_status");
    //else
    try {
        auto rst = func(path);
        umount(path.c_str());
        std::filesystem::remove(path);
        return rst;
    }
    catch (...) {
        umount(path.c_str());
        std::filesystem::remove(path);
        throw;
    }
}

static void grub_mkimage(const std::filesystem::path& boot_partition_dir)
{
    auto efi_boot = boot_partition_dir / "efi/boot";
    std::filesystem::create_directories(efi_boot);
    // install EFI bootloader
    exec_command("grub-mkimage", {"-p", "/boot/grub", "-o", (efi_boot / "bootx64.efi").string(), "-O", "x86_64-efi", 
        "xfs","btrfs","fat","part_gpt","part_msdos","normal","linux","echo","all_video","test","multiboot","multiboot2","search","sleep","iso9660","gzio",
        "lvm","chain","configfile","cpuid","minicmd","gfxterm_background","png","font","terminal","squash4","serial","loopback","videoinfo","videotest",
        "blocklist","probe","efi_gop","efi_uga"/*, "keystatus"*/});
}

static void grub_install(const std::filesystem::path& boot_partition_dir, const std::filesystem::path& disk)
{
    exec_command("grub-install", {"--target=i386-pc", "--recheck", std::string("--boot-directory=") + (boot_partition_dir / "boot").string(),
        "--modules=xfs btrfs fat part_msdos normal linux linux16 echo all_video test multiboot multiboot2 search sleep gzio lvm chain configfile cpuid minicmd font terminal serial squash4 loopback videoinfo videotest blocklist probe gfxterm_background png keystatus",
        disk.string()});
}

namespace install {

bool install(const std::filesystem::path& disk, uint64_t least_size, 
    const std::filesystem::path& system_img,
    const std::map<std::string,std::string>& grub_vars,
    std::function<void(double)> progress = [](auto){}, 
    std::function<void(const std::string&)> message = [](auto){})
{
    progress(0.01);
    const auto disks = enum_usable_disks(least_size);
    auto canonical_disk_name = std::filesystem::canonical(disk).string();
    auto disk_i = disks.find(canonical_disk_name);
    if (disk_i == disks.end()) throw std::runtime_error(canonical_disk_name + " is not a usable disk");
    auto size = disk_i->second.size;
    auto log_sec = disk_i->second.log_sec;

    std::vector<std::string> parted_args = {"--script", disk.string()};
    bool bios_compatible = (size <= 2199023255552L/*2TiB*/ && log_sec == 512);
    parted_args.push_back(bios_compatible? "mklabel msdos" : "mklabel gpt");
    bool has_secondary_partition = size >= 9000000000L; // more than 9GB

    if (has_secondary_partition) {
        parted_args.push_back("mkpart primary fat32 1MiB 8GiB");
        parted_args.push_back("mkpart primary btrfs 8GiB -1");
    } else {
        message("Warning: Data area won't be created due to too small disk");
        parted_args.push_back("mkpart primary fat32 1MiB -1");
    }
    parted_args.push_back("set 1 boot on");
    if (bios_compatible) {
        parted_args.push_back("set 1 esp on");
    }

    message("Creating partitions...");
    std::flush(std::cout);
    exec_command("parted", parted_args);
    exec_command("udevadm", {"settle"});
    message("Creating partitions done.");

    progress(0.03);

    auto _boot_partition = get_partition(disk, 1);
    if (!_boot_partition) {
        message("Error: Unable to determine boot partition");
        throw std::runtime_error("No boot partition");
    }
    //else
    auto boot_partition = _boot_partition.value();

    message("Formatting boot partition with FAT32");
    exec_command("mkfs.vfat",{"-F","32",boot_partition});

    progress(0.05);

    message("Mouning boot partition...");
    std::flush(std::cout);
    bool done = with_tempmount<bool>(boot_partition, "vfat", MS_RELATIME, "fmask=177,dmask=077", 
        [&disk,&system_img,&grub_vars,bios_compatible,&progress,&message](auto mnt) {
        message("Done");

        progress(0.07);

        message("Installing UEFI bootloader");
        grub_mkimage(mnt);
        if (bios_compatible) {
            message("Installing BIOS bootloader");
            grub_install(mnt, disk);
        } else {
            message("This system will be UEFI-only as this disk cannot be treated by BIOS");
        }

        progress(0.09);

        auto grub_dir = mnt / "boot/grub";
        std::filesystem::create_directories(grub_dir);
        message("Creating boot configuration file");
        {
            std::ofstream grubcfg(grub_dir / "grub.cfg");
            if (grubcfg.fail()) throw std::runtime_error("ofstream('/boot/grub/grub.cfg')");
            grubcfg << "insmod echo\ninsmod linux\ninsmod cpuid\n"
                << "set BOOT_PARTITION=$root\n"
                << "loopback loop /system.img\n"
                << "set root=loop\nset prefix=($root)/boot/grub\nnormal"
                << std::endl;
        }
        if (grub_vars.size() > 0) {
            std::ofstream systemcfg(mnt / "system.cfg");
            if (systemcfg.fail()) throw std::runtime_error("ofstream('/system.cfg')");
            for (const auto& [k,v] : grub_vars) {
                systemcfg << "set " << k << '=' << v << std::endl;
            }
        }

        progress(0.10);

        message("Copying system file");
        char buf[1024 * 1024]; // 1MB buffer
        FILE* f1 = fopen(system_img.c_str(), "r");
        if (!f1) throw std::runtime_error("Unable to open system file");
        //else
        struct stat statbuf;
        if (fstat(fileno(f1), &statbuf) < 0 || statbuf.st_size == 0) {
            fclose(f1);
            throw std::runtime_error("Unable to stat system file");
        }
        FILE* f2 = fopen((mnt / "system.img").c_str(), "w");
        size_t r;
        size_t cnt = 0;
        uint8_t percentage = 0;
        bool done = true;
        do {
            r = fread(buf, 1, sizeof(buf), f1);
            fwrite(buf, 1, r, f2);
            fflush(f2);
            fdatasync(fileno(f2));
            cnt += r;
            std::flush(std::cout);
            uint8_t new_percentage = cnt * 100 / statbuf.st_size;
            if (new_percentage > percentage) {
                percentage = new_percentage;
                std::flush(std::cout);
            }
            progress((double)cnt / statbuf.st_size * 0.8 + 0.1);
        } while (r == sizeof(buf));
        std::cout << std::endl;
        fclose(f1);
        fclose(f2);
        message("Unmounting boot partition...");
        std::flush(std::cout);
        return done;
    });
    message("Done");
    if (!done) return false;
    //else

    progress(0.90);

    if (has_secondary_partition) {
        message("Constructing data area");
        auto secondary_partition = get_partition(disk, 2);
        if (secondary_partition) {
            auto boot_partition_uuid = get_partition_uuid(boot_partition);
            if (boot_partition_uuid) {
                auto label = std::string("data-") + boot_partition_uuid.value();
                auto partition_name = secondary_partition.value();
                message("Formatting partition for data area with BTRFS...");
                std::flush(std::cout);
                exec_command("mkfs.btrfs", {"-q", "-L", label, "-f", partition_name.string()});
                message("Done");
            } else {
                message("Warning: Unable to get UUID of boot partition. Data area won't be created");
            }
        } else {
            message("Warning: Unable to determine partition for data area. Data area won't be created");
        }
    }
    progress(1.00);

    return true;
}

int install(const std::filesystem::path& disk, const std::filesystem::path& system_image,
    bool text_mode, bool installer)
{
    uint64_t least_size = 1024 * 1024 * 1024 * 8LL/*8GB*/;
    std::map<std::string,std::string> grub_vars;
    if (text_mode) grub_vars["default"] = "text";
    if (installer) grub_vars["systemd_unit"] = "installer.target";

    return install(disk, least_size, system_image, grub_vars, [](auto){}, [](const std::string& message) {
        std::cout << message << std::endl;
    }) == 0;
}

int show_usable_disks()
{
    uint64_t least_size = 1024 * 1024 * 1024 * 8LL/*8GB*/;
    auto disks = enum_usable_disks(least_size);

    if (disks.size() == 0) {
        std::cerr << "Sorry, no usable disks found." << std::endl;
        return 1;
    }

    std::shared_ptr<libscols_table> table(scols_new_table(), scols_unref_table);
    if (!table) throw std::runtime_error("scols_new_table() failed");
    scols_table_new_column(table.get(), "NAME", 0.1, 0);
    scols_table_new_column(table.get(), "MODEL", 0.1, 0);
    scols_table_new_column(table.get(), "SIZE", 0.1, SCOLS_FL_RIGHT);
    scols_table_new_column(table.get(), "TRAN", 0.1, 0);
    scols_table_new_column(table.get(), "LOG-SEC", 0.1, SCOLS_FL_RIGHT);
    auto sep = scols_table_new_line(table.get(), NULL);
    scols_line_set_data(sep, 0, "----------");
    scols_line_set_data(sep, 1, "-------------------------");
    scols_line_set_data(sep, 2, "------");
    scols_line_set_data(sep, 3, "------");
    scols_line_set_data(sep, 4, "-------");

    for (const auto& [dev,disk]:disks) {
        auto line = scols_table_new_line(table.get(), NULL);
        scols_line_set_data(line, 0, dev.c_str());
        scols_line_set_data(line, 1, disk.model.value_or("-").c_str());
        scols_line_set_data(line, 2, human_readable(disk.size).c_str());
        scols_line_set_data(line, 3, disk.tran.value_or("-").c_str());
        scols_line_set_data(line, 4, std::to_string(disk.log_sec).c_str());
    }

    scols_print_table(table.get());
    return 0;
}

int create_install_media(const std::filesystem::path& disk, const std::filesystem::path& system_image)
{
    const auto disks = enum_usable_disks(1024L * 1024 * 1024 * 3/*3GB*/);
    auto canonical_disk_name = std::filesystem::canonical(disk).string();
    auto disk_i = disks.find(canonical_disk_name);
    if (disk_i == disks.end()) throw std::runtime_error(canonical_disk_name + " is not a usable disk");

    if (disk_i->second.size > 2199023255552L/*2TiB*/) throw std::runtime_error("Disk is too large for FAT32.");
    if (!std::filesystem::exists(system_image)) throw std::runtime_error("System image file does not exist.");

    std::vector<std::string> parted_args = {"parted", "--script", disk.string()};
    bool bios_compatible = (disk_i->second.log_sec == 512);
    parted_args.push_back(bios_compatible? "mklabel msdos" : "mklabel gpt");
    parted_args.push_back("mkpart primary fat32 1MiB -1");
    parted_args.push_back("set 1 boot on");
    if (bios_compatible) parted_args.push_back("set 1 esp on");
    exec_command("parted", parted_args);
    exec_command("udevadm", {"settle"});
    auto boot_partition_path = get_partition(disk, 1);
    if (!boot_partition_path) throw std::runtime_error("Unable to determine created boot partition");
    exec_command("mkfs.vfat", {"-F", "32", "-n", "WBINSTALL", boot_partition_path.value()});

    return with_tempmount<int>(boot_partition_path.value(), "vfat", MS_RELATIME, "fmask=177,dmask=077", 
        [&disk,&system_image,&boot_partition_path,bios_compatible](const std::filesystem::path& mnt) {
        std::filesystem::copy(system_image, mnt / "system.img");
        std::ofstream f(mnt / "system.cfg");
        if (!f) throw std::runtime_error("system.cfg cannot be opened");
        f << "set systemd_unit=\"installer.target\"" << std::endl;

        grub_mkimage(boot_partition_path.value());
        //else
        if (bios_compatible) grub_install(boot_partition_path.value(), disk);
        // create boot config file
        auto grub_dir = boot_partition_path.value() / "boot/grub";
        std::filesystem::create_directories(grub_dir);
        {
            std::ofstream grubcfg(grub_dir / "grub.cfg");
            if (grubcfg) {
                grubcfg << "insmod echo\ninsmod linux\ninsmod cpuid\n"
                    << "set BOOT_PARTITION=$root\n"
                    << "loopback loop /system.img\n"
                    << "set root=loop\nset prefix=($root)/boot/grub\nnormal"
                    << std::endl;
            } else {
                std::cout << "Writing grub.cfg failed." << std::endl;
                return 1;
            }
        }
        return 0;
    });
}

} // namespace install

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
    //return install::install("disk", std::nullopt, true, false); 
    return install::show_usable_disks();
}
#endif

