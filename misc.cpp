#include <sys/wait.h>

#include <memory>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <wayland-client.h>

std::string human_readable(uint64_t size, double k/* = 1024.0*/)
{
    char buf[32];
    char au = 'K';

    float s = size / k;

    if (s >= k) {
        s /= k;
        au = 'M';
    }
    if (s >= k) {
        s /= k;
        au = 'G';
    }
    if (s >= k) {
        s /= k;
        au = 'T';
    }
    if (s >= k) {
        s /= k;
        au = 'P';
    }
    sprintf(buf, "%.1f%c", s, au);
    return std::string(buf);
}

bool wayland_ping(bool wait)
{
    std::shared_ptr<wl_display> display(wl_display_connect(NULL), 
        [](auto ptr){if (ptr) wl_display_disconnect(ptr);});

    if (!display) {
        throw std::runtime_error("Can't connect to display");
    }

    //else
    bool output = false;
    while (true) {
        struct wl_registry *registry = wl_display_get_registry(display.get());
        const struct wl_registry_listener registry_listener = {
            [](void *data, struct wl_registry *, uint32_t,const char *interface, uint32_t){
                if (std::string(interface) == "wl_output") *((bool *)data) = true;
            }, NULL
        };
        wl_registry_add_listener(registry, &registry_listener, &output);

        wl_display_dispatch(display.get());
        wl_display_roundtrip(display.get());

        if (!wait || output) break;
        // else
        sleep(3);
    }

    return output;
}

int generate_rdp_cert()
{
    static const std::filesystem::path key("/etc/ssl/private/rdp.key");
    static const std::filesystem::path cert("/etc/ssl/certs/rdp.crt");
    if (std::filesystem::exists(key) && std::filesystem::exists(cert)) return 0;
    auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        _exit(execlp("openssl",
            "openssl", "req", "-x509", "-newkey", "rsa:4096", 
            "-keyout", key.c_str(), "-out", cert.c_str(), 
            "-sha256", "-nodes",  "-subj", "/", "-days", "36500", 
            NULL
        ));
    }
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus)) throw std::runtime_error("openssl command terminated abnormally");
    return WEXITSTATUS(wstatus);
}

/**
 * /sys/block/[storage_device_name]/device/wwid ファイルの内容をすべて表示する。
 */
void list_wwid()
{
    std::filesystem::path sys_block("/sys/block");
    for (auto &p: std::filesystem::directory_iterator(sys_block)) {
        auto disk_name = p.path().filename();
        std::filesystem::path device(p.path() / "device");
        if (!std::filesystem::is_directory(device)) continue;
        std::filesystem::path wwid(device / "wwid");
        if (!std::filesystem::exists(wwid)) continue;
        std::ifstream ifs(wwid);
        std::string line;
        std::getline(ifs, line);
        std::cout << disk_name.string() << ": " << line << std::endl;
    }
}

#ifdef __VSCODE_ACTIVE_FILE__
int main()
{
    wayland_ping(false);
}
#endif // __TEST__