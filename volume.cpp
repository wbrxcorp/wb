/**
 * @file volume.cpp
 * @brief Volume management
 * @author Walbrix Corporation
 * @date 2019-2020
 * @details This file is part of the Walbrix Virtual Machine Manager.
 */
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#include <iostream>
#include <fstream>
#include <map>

#include <libsmartcols/libsmartcols.h>
#include <libmount/libmount.h>
#include <blkid/blkid.h>
#include <btrfsutil.h>

#include "misc.h"
#include "volume.h"

/**
 * @brief Get UUID of the partition
 * @param partition Path to the partition
 * @return UUID of the partition or std::nullopt if failed
 */
static std::optional<std::string> get_partition_uuid(const std::filesystem::path& partition)
{
  blkid_cache cache;
  if (blkid_get_cache(&cache, "/dev/null") != 0) throw std::runtime_error("blkid_get_cache() failed");
  // else
  std::optional<std::string> rst = std::nullopt;
  if (blkid_probe_all(cache) == 0) {
    auto tag_value = blkid_get_tag_value(cache, "UUID", partition.c_str());
    if (tag_value) rst = tag_value;
  }
  blkid_put_cache(cache);
  return rst;
}

/**
 * @brief Mount filesystem
 * @param source Source device
 * @param mountpoint Mount point
 * @param fstype Filesystem type (default: auto)
 * @param mountflags Mount flags (default: MS_RELATIME)
 * @param data Mount options (default: "")
 * @return 0 if success, 1 if already mounted, -1 if failed
 */
static int mount(const std::string& source,
  const std::filesystem::path& mountpoint,
  const std::string& fstype = "auto", unsigned int mountflags = MS_RELATIME,
  const std::string& data = "")
{
    std::shared_ptr<libmnt_context> ctx(mnt_new_context(), mnt_free_context);
    mnt_context_set_fstype_pattern(ctx.get(), fstype.c_str());
    mnt_context_set_source(ctx.get(), source.c_str());
    mnt_context_set_target(ctx.get(), mountpoint.c_str());
    mnt_context_set_mflags(ctx.get(), mountflags);
    mnt_context_set_options(ctx.get(), data.c_str());

    int rst = mnt_context_mount(ctx.get());
    if (rst != 0) {
        if (rst > 1) perror("mnt_context_mount");
        return rst;
    }
    //else
    return mnt_context_get_status(ctx.get()) == 1? 0 : -1;
}

struct Volume {
    std::string name;
    bool online = false;
    std::filesystem::path path;
    std::string device_or_uuid;
    std::optional<std::string> fstype;
    std::optional<uint64_t> size;
    std::optional<uint64_t> free;
};

static std::optional<Volume> get_volume(const std::filesystem::path& path)
{
    if (!std::filesystem::is_directory(path)) return {};
    auto name =  path.filename().string();
    if (name[0] != '@') return {};
    name.replace(name.begin(), name.begin() + 1, "");

    Volume vol;
    vol.name = name;

    auto device = volume::get_source_device_from_mountpoint(path);
    auto uuid_file = path / ".uuid";
    if (!device && !std::filesystem::exists(uuid_file)) return {};

    if (device) {
        vol.online = true;
        vol.path = path;
        vol.device_or_uuid = device.value().first.c_str();
        vol.fstype = device.value().second.c_str();

        struct statvfs vfs;
        if (statvfs(path.c_str(), &vfs) == 0) {
            uint64_t blocksize = vfs.f_frsize? vfs.f_frsize : vfs.f_bsize; // https://github.com/coreutils/gnulib/blob/master/lib/fsusage.c#L124
            vol.size = blocksize * vfs.f_blocks;
            vol.free = blocksize * vfs.f_bfree;
        }
    } else {
        std::ifstream f(uuid_file);
        if (!f) return {};
        f >> vol.device_or_uuid;
    }
    return vol;
}

static std::optional<Volume> get_volume(const std::filesystem::path& vm_root, const std::string& name)
{
    auto path = vm_root / ("@" + name);
    return get_volume(path);
}

/**
 * @brief Get list of volumes
 * @param vm_root Root directory of VMs
 * @return Map of volumes 
 */
static std::map<std::string,Volume> get_volume_list(const std::filesystem::path& vm_root)
{
    std::map<std::string,Volume> volumes;
    for (const auto& dir : std::filesystem::directory_iterator(vm_root)) {
        auto vol = get_volume(dir.path());
        if (!vol) continue;
        volumes[vol->name] = *vol;
    }
    return volumes;
}

/**
 * @brief Take snapshot of specified btrfs mountpoint
 * @param path Path to the mountpoint
 * @return Path to the snapshot
 */
static std::filesystem::path snapshot(const std::filesystem::path& path)
{
    if (btrfs_util_is_subvolume(path.c_str()) != BTRFS_UTIL_OK) {
        throw std::runtime_error(path.string() + " is offline or not a btrfs volume");
    }
    //else
    auto head = path / ".snapshots/head";

    // create snapshot
    if (btrfs_util_is_subvolume(head.c_str()) == BTRFS_UTIL_OK) {
        struct btrfs_util_subvolume_info subvol;
        auto rst = btrfs_util_subvolume_info(head.c_str(), 0, &subvol);
        if (rst != BTRFS_UTIL_OK) {
            throw std::runtime_error("Inspecting subvolume " + head.string() + " failed(" + btrfs_util_strerror(rst) + ")");
        }
        //else

        static const char* DOWSTR[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        auto dow = path / ".snapshots" / DOWSTR[localtime(&subvol.otime.tv_sec)->tm_wday];
        if (btrfs_util_is_subvolume(dow.c_str()) == BTRFS_UTIL_OK) {
            btrfs_util_delete_subvolume(dow.c_str(), BTRFS_UTIL_DELETE_SUBVOLUME_RECURSIVE);
            std::cerr << "Snapshot " << dow << " deleted" << std::endl;
        }
        std::filesystem::rename(head, dow);
        std::cerr << "Snapshot " << head << " renamed to " << dow << std::endl;
    }
    std::filesystem::create_directory(path / ".snapshots");
    auto rst = btrfs_util_create_snapshot(path.c_str(), head.c_str(), BTRFS_UTIL_CREATE_SNAPSHOT_READ_ONLY, NULL, NULL);
    if (rst != BTRFS_UTIL_OK) {
        throw std::runtime_error("Creating readonly snapshot " + head.string() + " failed(" + btrfs_util_strerror(rst) + ")");
    }
    sync();
    return head;
}

/**
 * @brief Backup specified btrfs mountpoint
 * @param path Path to the mountpoint
 */
static void backup(const std::filesystem::path& path)
{
    std::filesystem::path head = snapshot(path);
    std::cout << "Snapshot " << head << " created" << std::endl;

    auto backup_link = path / ".backup";
    if (!std::filesystem::is_symlink(backup_link)) return;

    auto backup_dir = std::filesystem::weakly_canonical(backup_link);
    if (!std::filesystem::is_directory(backup_dir)) {
        throw std::runtime_error("Backup link " + backup_link.string() + " broken");
    }
    //else

    std::cout << "Backing up snapshot " << head << " to " << backup_dir << " using rdiff-backup..." << std::endl;
    auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        if (execlp("rdiff-backup", "rdiff-backup", "--preserve-numerical-ids", "--print",
            "--exclude", (head / ".trash").c_str(),
            "--exclude", (head / ".snapshots").c_str(),
            head.c_str(), backup_dir.c_str(), NULL) < 0) exit(-1);
    }
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) throw std::runtime_error("waitpid() failed");
    if (!WIFEXITED(wstatus)) throw std::runtime_error("rdiff-backup terminated");
    if (WEXITSTATUS(wstatus) != 0) throw std::runtime_error("rdiff-backup failed");

    pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        if (execlp("rdiff-backup", "rdiff-backup",
            "--remove-older-than", "1W", "--force", backup_dir.c_str(), NULL) < 0) exit(-1);
    }
    if (waitpid(pid, &wstatus, 0) < 0) throw std::runtime_error("waitpid() failed");
    if (!WIFEXITED(wstatus)) throw std::runtime_error("rdiff-backup --remove-older-than terminated");
    if (WEXITSTATUS(wstatus) != 0) throw std::runtime_error("rdiff-backup --remove-older-than failed");
}

/**
 * @brief Clean trash of specified volume
 * @param vm_root Root directory of VMs
 * @param volume_name Name of the volume
 * @return true if trash is cleaned, false if no trash exists
 */
static bool clean(const std::filesystem::path& vm_root, const std::string& volume_name)
{
    auto volume_dir = volume::get_volume_dir(vm_root, volume_name);
    if (!volume_dir) throw std::runtime_error("No such volume: " + volume_name);
    //else
    return std::filesystem::remove_all(*volume_dir / ".trash") > 0;
}

namespace volume {

std::optional<std::pair<std::filesystem::path,std::string/*fstype*/>> get_source_device_from_mountpoint(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) return std::nullopt;
    // else
    std::shared_ptr<libmnt_table> tb(mnt_new_table_from_file("/proc/self/mountinfo"),mnt_unref_table);
    if (!tb) throw std::runtime_error("Cannot open /proc/self/mountinfo");
    std::shared_ptr<libmnt_cache> cache(mnt_new_cache(), mnt_unref_cache);
    mnt_table_set_cache(tb.get(), cache.get());

    libmnt_fs* fs = mnt_table_find_target(tb.get(), path.c_str(), MNT_ITER_BACKWARD);
    return fs? std::optional(std::make_pair(mnt_fs_get_srcpath(fs), mnt_fs_get_fstype(fs))) : std::nullopt;
}

std::optional<std::filesystem::path> get_volume_dir(const std::filesystem::path& vm_root, const std::string& volume_name)
{
    auto volume_dir = vm_root / ("@" + volume_name);
    if (volume_name == "default") return std::filesystem::is_directory(volume_dir)? std::make_optional(volume_dir) : std::nullopt;
    //else
    return volume::get_source_device_from_mountpoint(volume_dir)? std::make_optional(volume_dir) : std::nullopt;
}

int add(const std::filesystem::path& vm_root, const std::string& name, const std::filesystem::path& device)
{
    if (name == "default") throw std::runtime_error("Default volume cannot be modified");

    if (!std::filesystem::exists(device) || !std::filesystem::is_block_file(device)) {
        throw std::runtime_error(device.string() + " does not exist(or is not a block device)");
    }
    auto uuid = get_partition_uuid(device).value_or("");
    if (uuid == "") throw std::runtime_error(device.string() + " has no UUID(not formatted?)");

    auto volume_path = vm_root / ("@" + name);

    if (get_source_device_from_mountpoint(volume_path)) {
        throw std::runtime_error(name + " has already been mounted");
    }

    auto uuid_file = volume_path / ".uuid";
    if (std::filesystem::exists(uuid_file)) {
        throw std::runtime_error(name + " has already been associated to a partition");
    }

    std::filesystem::create_directories(volume_path);

    auto subvolume_file = volume_path / ".subvolume";

    if (std::filesystem::exists(subvolume_file)) {
        std::filesystem::remove(subvolume_file);
    }

    {
        std::ofstream f(uuid_file);
        if (!f) throw std::runtime_error("Failed to open " + uuid_file.string());
        f << uuid;
        if (f.bad()) throw std::runtime_error("Failed writing to " + uuid_file.string());
        f.close();
        if (f.fail()) throw std::runtime_error("Failed to close " + uuid_file.string());
    }

    if (mount(std::string("UUID=") + uuid, volume_path) != 0) {
        std::filesystem::remove_all(volume_path);
        throw std::runtime_error("Failed to mount UUID=" + uuid + " on " + volume_path.string());
    }

    return 0;
}

int remove(const std::filesystem::path& vm_root, const std::string& name)
{
    if (name == "default") throw std::runtime_error("Default volume cannot be modified");

    auto volume_path = vm_root / ("@" + name);

    if (!std::filesystem::exists(volume_path)) {
        throw std::runtime_error("Volume " + name + " does not exist");
    }
    if (get_source_device_from_mountpoint(volume_path)) {
        auto rst = umount(volume_path.c_str());
        if (rst < 0) {
            throw std::runtime_error("Unable to unmount " + volume_path.string());
        }
    }
    std::filesystem::remove_all(volume_path);

    return 0;
}

int scan(const std::filesystem::path& vm_root)
{
    for (const auto& dir : std::filesystem::directory_iterator(vm_root)) {
        if (!dir.is_directory()) continue;
        const auto& path = dir.path();
        auto name =  path.filename().string();
        if (name[0] != '@') continue;
        name.replace(name.begin(), name.begin() + 1, "");
        if (name == "default") continue;

        if (get_source_device_from_mountpoint(path)) continue; // already mounted

        auto uuid_file = path / ".uuid";
        if (!std::filesystem::exists(uuid_file) || !std::filesystem::is_regular_file(uuid_file)) continue;

        std::string uuid;
        {
            std::ifstream f(uuid_file);
            if (!f) continue;
            f >> uuid;
        }

        if (mount(std::string("UUID=") + uuid, path) == 0) {
            std::cout << "Volume " + name + "(UUID=" + uuid + ") mounted on " + path.string() << std::endl;
        } else {
            std::cerr << "Volume " + name + "(UUID=" + uuid + ") couldn't be mounted." << std::endl;
        }
    }
    return 0;
}

int list(const std::filesystem::path& vm_root, const ListOptions& options/* = {}*/)
{
    auto volumes = get_volume_list(vm_root);
    if (options.names_only) {
        for (const auto& volume : volumes) {
            if (options.online_only && !volume.second.online) continue;
            std::cout << volume.second.name << std::endl;
        }
        return 0;
    }
    //else

    std::shared_ptr<libscols_table> table(scols_new_table(), scols_unref_table);
    if (!table) throw std::runtime_error("scols_new_table() failed");
    scols_table_new_column(table.get(), "ONLINE", 0.1, SCOLS_FL_RIGHT);
    scols_table_new_column(table.get(), "NAME", 0.1, 0);
    scols_table_new_column(table.get(), "PATH", 0.1, 0);
    scols_table_new_column(table.get(), "DEVICE | UUID", 0.1, 0);
    scols_table_new_column(table.get(), "FSTYPE", 0.1, 0);
    scols_table_new_column(table.get(), "SIZE", 0.1, SCOLS_FL_RIGHT);
    scols_table_new_column(table.get(), "FREE", 0.1, SCOLS_FL_RIGHT);
    auto sep = scols_table_new_line(table.get(), NULL);
    scols_line_set_data(sep, 0, "------");
    scols_line_set_data(sep, 1, "--------");
    scols_line_set_data(sep, 2, "----------------");
    scols_line_set_data(sep, 3, "------------------------------------");
    scols_line_set_data(sep, 4, "------");
    scols_line_set_data(sep, 5, "-------");
    scols_line_set_data(sep, 6, "-------");

    for (const auto& i : volumes) {
        if (options.online_only && !i.second.online) continue;
        auto line = scols_table_new_line(table.get(), NULL);
        //else
        if (i.second.online) scols_line_set_data(line, 0, "*");
        scols_line_set_data(line, 1, i.first.c_str());
        scols_line_set_data(line, 2, i.second.path.c_str());
        scols_line_set_data(line, 3, i.second.device_or_uuid.c_str());
        if (i.second.fstype) scols_line_set_data(line, 4, i.second.fstype.value().c_str());
        if (i.second.size) scols_line_set_data(line, 5, human_readable(i.second.size.value()).c_str());
        if (i.second.free) scols_line_set_data(line, 6, human_readable(i.second.free.value()).c_str());
    }

    scols_print_table(table.get());
    return 0;
}

int snapshot(const std::filesystem::path& vm_root, const std::string& volume_name)
{
    auto volume = get_volume(vm_root, volume_name);
    if (!volume) {
        std::cerr << "Volume " + volume_name + " does not exist" << std::endl;
        return 1;
    }
    auto head = ::snapshot(volume->path);
    std::cout << head.string() << std::endl;
    return 0;
}

int backup(const std::filesystem::path& vm_root)
{
    auto volumes = get_volume_list(vm_root);
    bool all_success = true;
    for (const auto& volume : volumes) {
        if (!volume.second.online) continue; // volume not mounted
        try {
            ::backup(volume.second.path);
        }
        catch (const std::runtime_error& e) {
            std::cerr << e.what() << std::endl;
            all_success = false;
        }
    }

    return all_success? 0 : 1;
}

int clean(const std::filesystem::path& vm_root, const std::optional<std::string>& volume_name)
{
    if (volume_name) {
        ::clean(vm_root, *volume_name);
        return 0;
    }
    // else 
    auto volumes = get_volume_list(vm_root);
    bool all_success = true;
    for (const auto& volume : volumes) {
        if (!volume.second.online) continue; // volume not mounted
        try {
            ::clean(vm_root, volume.first);
        }
        catch (const std::runtime_error& e) {
            std::cerr << e.what() << std::endl;
            all_success = false;
        }
    }
    return all_success? 0 : 1;
}

} // namespace volume

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
}
#endif