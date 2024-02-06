#include "model_downloader.h"

#include "models.h"
#include "utils/crypto_utils.h"

#include <elzip/elzip.hpp>
#include <spdlog/spdlog.h>

#include <sstream>

#ifndef _WIN32
// Required for MSG_NOSIGNAL and SO_NOSIGPIPE
#include <sys/socket.h>
#include <sys/types.h>
#endif

#ifdef MSG_NOSIGNAL
#define CPPHTTPLIB_SEND_FLAGS MSG_NOSIGNAL
#endif
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace fs = std::filesystem;

namespace dorado::models {

namespace {

namespace urls {
const std::string URL_ROOT = "https://cdn.oxfordnanoportal.com";
const std::string URL_PATH = "/software/analysis/dorado/";
}  // namespace urls

std::string calculate_checksum(std::string_view data) {
    // Hash the data.
    const auto hash = utils::crypto::sha256(data);

    // Stringify it.
    std::ostringstream checksum;
    checksum << std::hex;
    checksum.fill('0');
    for (unsigned char byte : hash) {
        checksum << std::setw(2) << static_cast<int>(byte);
    }
    return std::move(checksum).str();
}

void set_ssl_cert_file() {
#ifndef _WIN32
    // Allow the user to override this.
    if (getenv("SSL_CERT_FILE") != nullptr) {
        return;
    }

    // Try and find the cert location.
    const char* ssl_cert_file = nullptr;
#ifdef __linux__
    // We link to a static Ubuntu build of OpenSSL so it's expecting certs to be where Ubuntu puts them.
    // For other distributions they may not be in the same place or have the same name.
    if (fs::exists("/etc/os-release")) {
        std::ifstream os_release("/etc/os-release");
        std::string line;
        while (std::getline(os_release, line)) {
            if (line.rfind("ID=", 0) == 0) {
                if (line.find("ubuntu") != line.npos || line.find("debian") != line.npos) {
                    // SSL will pick the right one.
                    return;
                } else if (line.find("centos") != line.npos) {
                    ssl_cert_file = "/etc/ssl/certs/ca-bundle.crt";
                }
                break;
            }
        }
    }
    if (!ssl_cert_file) {
        spdlog::warn(
                "Unknown certs location for current distribution. If you hit download issues, "
                "use the envvar `SSL_CERT_FILE` to specify the location manually.");
    }

#elif defined(__APPLE__)
    // The homebrew built OpenSSL adds a dependency on having homebrew installed since it looks in there for certs.
    // The default conan OpenSSL is also misconfigured to look for certs in the OpenSSL build folder.
    // macOS provides certs at the following location, so use those in all cases.
    ssl_cert_file = "/etc/ssl/cert.pem";
#endif

    // Update the envvar.
    if (ssl_cert_file) {
        spdlog::info("Assuming cert location is {}", ssl_cert_file);
        setenv("SSL_CERT_FILE", ssl_cert_file, 1);
    }
#endif  // _WIN32
}

auto create_client() {
    set_ssl_cert_file();

    auto http = std::make_unique<httplib::Client>(urls::URL_ROOT);
    http->set_follow_location(true);
    http->set_connection_timeout(20);

    const char* proxy_url = getenv("dorado_proxy");
    const char* ps = getenv("dorado_proxy_port");

    int proxy_port = 3128;
    if (ps) {
        proxy_port = atoi(ps);
    }

    if (proxy_url) {
        spdlog::info("using proxy: {}:{}", proxy_url, proxy_port);
        http->set_proxy(proxy_url, proxy_port);
    }

    http->set_socket_options([](socket_t sock) {
#ifdef __APPLE__
        // Disable SIGPIPE signal generation since it takes down the entire process
        // whereas we can more gracefully handle the EPIPE error.
        int enabled = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<char*>(&enabled),
                   sizeof(enabled));
#else
        (void)sock;
#endif
    });

    return http;
}

}  // namespace

ModelDownloader::ModelDownloader(fs::path directory)
        : m_client(create_client()), m_directory(std::move(directory)) {}

ModelDownloader::~ModelDownloader() = default;

bool ModelDownloader::download(const std::string& model, const ModelInfo& info) {
    auto archive = m_directory / (model + ".zip");

    // Try and download using httplib, falling back on curl.
    if (!download_httplib(model, info, archive) && !download_curl(model, info, archive)) {
        return false;
    }

    // Extract it.
    extract(archive);
    return true;
}

std::string ModelDownloader::get_url(const std::string& model) const {
    return urls::URL_ROOT + urls::URL_PATH + model + ".zip";
}

void ModelDownloader::extract(const fs::path& archive) const {
    elz::extractZip(archive, m_directory);
    fs::remove(archive);
}

bool ModelDownloader::download_httplib(const std::string& model,
                                       const ModelInfo& info,
                                       const fs::path& archive) {
    spdlog::info(" - downloading {} with httplib", model);
    httplib::Result res = m_client->Get(get_url(model));
    if (!res) {
        spdlog::error("Failed to download {}: {}", model, to_string(res.error()));
        return false;
    }

    // Check that this matches the hash we expect.
    const auto checksum = calculate_checksum(res->body);
    if (checksum != info.checksum) {
        spdlog::error("Model download failed checksum validation: {} - {} != {}", model, checksum,
                      info.checksum);
        return false;
    }

    // Save it.
    std::ofstream output(archive.string(), std::ofstream::binary);
    output << res->body;
    output.close();
    return true;
}

bool ModelDownloader::download_curl(const std::string& model,
                                    const ModelInfo& info,
                                    const fs::path& archive) {
    spdlog::info(" - downloading {} with curl", model);

    // Note: it's safe to call system() here since we're only going to be called with known models.
    std::string args = "curl -L " + get_url(model) + " -o " + archive.string();
    errno = 0;
    int ret = system(args.c_str());
    if (ret != 0) {
        spdlog::error("Failed to download {}: ret={}, errno={}", model, ret, errno);
        return false;
    }

    // Load it back in and checksum it.
    // Note: there's TOCTOU issues here wrt the download above, and the file_size() call.
    std::ifstream output(archive.string(), std::ofstream::binary);
    std::string buffer;
    buffer.resize(fs::file_size(archive));
    output.read(buffer.data(), buffer.size());
    output.close();

    const auto checksum = calculate_checksum(buffer);
    if (checksum != info.checksum) {
        spdlog::error("Model download failed checksum validation: {} - {} != {}", model, checksum,
                      info.checksum);
        return false;
    }
    return true;
}

}  // namespace dorado::models
