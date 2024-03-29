#include <string.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/icmp6.h>
#include <sys/socket.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ext/stdio_filebuf.h> // for __gnu_cxx::stdio_filebuf

#include <qrencode.h>
#include <curl/curl.h>

#include <wghub.h>

#include "wg.h"

static const std::filesystem::path privkey_path("/etc/walbrix/privkey"), wireguard_dir("/etc/wireguard");
static const std::string base_url("https://hub.walbrix.net/wghub");
static const std::string tunnel_name("wg-walbrix");

static std::string get_privkey_b64()
{
    std::string privkey_b64;
    {
        std::ifstream f(privkey_path);
        if (!f) throw std::runtime_error("Unable to open " + privkey_path.string());
        // else
        f >> privkey_b64;
    }
    return privkey_b64;
}

static void print_qrcode(const QRcode *qrcode)
{
    static const char* white = "\033[48;5;231m";
    static const char* black = "\033[48;5;16m";
    static const char* reset = "\033[0m";

    auto vmargin = [](int qrwidth, int margin) {
        for (int y = 0; y < margin; y++) {
            std::cout << white;
            for (int x = 0; x < qrwidth + margin * 2/*left and right*/; x++) {
                std::cout << "  ";
            }
            std::cout << reset << std::endl;
        }
    };

    vmargin(qrcode->width, 2);

    for (int y = 0; y < qrcode->width; y++) {
        std::string buffer;
        const uint8_t* row = qrcode->data + (y * qrcode->width);
        buffer += white;
        buffer += "    "; // left margin(2)
        bool last = false;
        for (int x = 0; x < qrcode->width; x++) {
            if (row[x] & 1) {
                if (!last) {
                    buffer += black;
                    last = true;
                }
            } else if (last) {
                buffer += white;
                last = false;
            }
            buffer += "  ";
        }

        if (last) buffer += white;
        buffer += "    "; // right margin(2)
        buffer += reset;
        std::cout << buffer << std::endl;
    }

    vmargin(qrcode->width, 2);
}

static std::string strip_name_from_ssh_key(const std::string& ssh_key)
{
    auto first_spc = ssh_key.find_first_of(' ');
    if (first_spc == ssh_key.npos) throw std::runtime_error("No delimiter found in ssh key");
    auto last_spc = ssh_key.find_first_of(' ', first_spc + 1);
    return last_spc != ssh_key.npos? ssh_key.substr(0, last_spc) : ssh_key;
}

static size_t curl_callback(char *buffer, size_t size, size_t nmemb, void *f)
{
    (*((std::string*)f)) += std::string(buffer, size * nmemb);
    return size * nmemb;
}

static std::optional<std::string> get_wg_peer_address(const std::string& tunnel_name)
{
    int fd[2];
    if (pipe(fd) < 0) throw std::runtime_error("pipe() failed");

    auto pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");
    if (pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        _exit(execlp("wg", "wg", "show", "all", "allowed-ips", NULL));
    }
    //else
    close(fd[1]);

    std::optional<std::string> peer_address;
    {
        __gnu_cxx::stdio_filebuf<char> filebuf(fd[0], std::ios::in);
        std::istream f(&filebuf);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.starts_with(tunnel_name + "\t")) continue;
            line.erase(0, 11);
            auto delim_pos = line.find_first_of('\t');
            if (delim_pos == line.npos) continue;
            //else
            line.erase(0, delim_pos + 1);
            if (!line.ends_with("/128")) continue;
            line.resize(line.length() - 4);
            peer_address = line;
            break;
        }
    }

    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0)
        throw std::runtime_error("wg command failed");
    //else
    return peer_address;
}

static bool ping(const std::string& peer_address, uint16_t count = 5, bool verbose = false)
{
    int sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (sock < 0) throw std::runtime_error("socket() failed");
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    if (inet_pton(AF_INET6, peer_address.c_str(), &addr.sin6_addr) != 1)
        throw std::runtime_error("inet_pton() failed");
    struct icmp6_hdr icmp6;
    memset(&icmp6, 0, sizeof(icmp6));
    icmp6.icmp6_type = ICMP6_ECHO_REQUEST;
    icmp6.icmp6_code = 0;
    icmp6.icmp6_id = htons(getpid());
    int seq = 0;
    icmp6.icmp6_cksum = 0; // leave 0 to let kernel fill it

    char buf[1024];
    struct sockaddr_in6 peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);

    for (int i = 0; i < count; i++) {
        if (verbose) std::cout << "Sending echo request to " << peer_address << " with seq=" << seq << std::endl;
        icmp6.icmp6_seq = htons(seq++);
        if (sendto(sock, &icmp6, sizeof(icmp6), 0, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("sendto() failed");
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ret = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) throw std::runtime_error("select() failed");
        if (ret > 0) {
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&peer_addr, &peer_addr_len);
            if (n < 0) throw std::runtime_error("recvfrom() failed");
            //else
            if (verbose) std::cout << "Received echo reply with seq=" << ntohs(((struct icmp6_hdr*)buf)->icmp6_seq) << std::endl;
            return true;
        }
    }
    if (verbose) std::cout << "No echo reply received" << std::endl;
    return false;
}

namespace wg {

int genkey(bool force)
{
    if (std::filesystem::exists(privkey_path) && !force) {
        std::cout << "Key already exists. Use --force to overwrite" << std::endl;
        return 0;
    }

    if (privkey_path.has_parent_path()) {
        std::filesystem::create_directories(privkey_path.parent_path());
    }

    std::ofstream f(privkey_path);
    if (!f) throw std::runtime_error(privkey_path.string() + " couldn't be created");
    //else
    f << wghub::generate_private_key();

    return 0;
}


int pubkey(bool qrcode)
{
    auto privkey = get_privkey_b64();
    auto pubkey = wghub::get_public_key_from_private_key(privkey);

    if (qrcode) {
        std::shared_ptr<QRcode> qrcode(QRcode_encodeString(pubkey.c_str(), 0, QR_ECLEVEL_L, QR_MODE_8, 1), QRcode_free);
        if (!qrcode) throw std::runtime_error("Failed to generate QR code. " + std::string(strerror(errno)));
        print_qrcode(qrcode.get());
    } else {
        std::cout << pubkey << std::endl;
    }

    return 0;
}

int getconfig(bool accept_ssh_key)
{
    auto privkey = get_privkey_b64();
    auto pubkey = wghub::get_public_key_from_private_key(privkey);
    std::string url = wghub::get_authorization_url(base_url, pubkey);

    std::shared_ptr<CURL> curl(curl_easy_init(), curl_easy_cleanup);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    std::string buf;
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buf);
    auto res = curl_easy_perform(curl.get());
    if (res != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(res));
    }
    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code == 404) {
        std::cerr << "Not authorized yet" << std::endl;
        return 1;
    }
    if (http_code != 200) throw std::runtime_error("Server error: status code=" + std::to_string(http_code));

    auto config = wghub::decrypt_and_parse_client_config(buf, privkey);    

    std::filesystem::create_directories(wireguard_dir);
    {
        auto conf = wireguard_dir / "wg-walbrix.conf";
        std::ofstream f(conf);
        if (!f) throw std::runtime_error("Failed to open " + conf.string() + " for write");
        f << "[Interface]" << std::endl;
        f << "PrivateKey=" << privkey << std::endl;
        f << "Address=" << config.address << std::endl;
        f << "[Peer]" << std::endl;
        f << "PublicKey=" << config.peer_pubkey << std::endl;
        f << "endpoint=" << config.endpoint << std::endl;
        f << "AllowedIPs=" << config.peer_address << std::endl;
        f << "PersistentKeepalive=25" << std::endl;
    }

    if (config.serial) {
        auto pid = fork();
        if (pid < 0) throw std::runtime_error("fork() failed");
        if (pid == 0) {
            _exit(execlp("hostnamectl", "hostnamectl", "hostname", config.serial.value().c_str(), NULL));
        }
        int wstatus;
        if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0)
            std::cerr << "Setting hostname failed" << std::endl;
    }

    if (config.ssh_key && accept_ssh_key) {
        const char* HOME = getenv("HOME");
        if (!HOME) HOME = "/root";

        std::string ssh_key_strippted = strip_name_from_ssh_key(config.ssh_key.value());
        std::filesystem::path home(HOME);
        auto ssh_dir = home / ".ssh";
        if (!std::filesystem::exists(ssh_dir)) {
            std::filesystem::create_directory(ssh_dir);
            std::filesystem::permissions(ssh_dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
        }
        auto authorized_keys = ssh_dir / "authorized_keys";
        bool exists = false;
        if (std::filesystem::exists(authorized_keys) && std::filesystem::is_regular_file(authorized_keys)) {

            std::ifstream f(authorized_keys);
            if (f) {
                std::string line;
                while(std::getline(f, line)) {
                    if (ssh_key_strippted == strip_name_from_ssh_key(line)) {
                        exists = true;
                        break;
                    }
                }
            }
        }
        if (!exists) {
            std::ofstream f(authorized_keys, std::ios_base::app);
            if (!f) throw std::runtime_error("Failed to open " + authorized_keys.string() + " for append");
            //else
            f << config.ssh_key.value() << std::endl;
        }
    }

    return 0;
}

int notify(const std::string& uri)
{
    auto peer_address = get_wg_peer_address(tunnel_name);
    if (!peer_address) return 0; // just return success if wg-walbrix is not connected

    std::string url = "http://[" + (*peer_address) + "]" + (uri.starts_with('/')? "" : "/") + uri;
    std::string buf;
    std::shared_ptr<CURL> curl(curl_easy_init(), curl_easy_cleanup);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 3);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buf);
    curl_easy_perform(curl.get());

    return 0;
}

int ping(bool success_if_not_active, uint16_t count, bool verbose)
{
    auto peer_address = get_wg_peer_address(tunnel_name);
    if (!peer_address) {
        if (verbose) std::cout << "Tunnel '" << tunnel_name << "' is not active" << std::endl;
        return success_if_not_active? 0 : 1;
    }

    return ::ping(*peer_address, count, verbose)? 0 : 1;
}

} // namespace wg

#ifdef __VSCODE_ACTIVE_FILE__
int main(int argc, char* argv[])
{
    //return wg_notify({"wg-notify","/hoge/fuga"});
    return 0;
}
#endif
