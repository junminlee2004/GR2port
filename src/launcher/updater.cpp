// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <httplib.h>
#include <nlohmann/json.hpp>
// httplib brings zlib, whose names miniz would define again.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "launcher/updater.h"

namespace Gr2Port::Launcher::Updater {

namespace {

constexpr const char* GitHub = "https://api.github.com";
constexpr std::string_view Repository = "/repos/junminlee2004/GR2port";
// A release holds a zip for each system, which its name's ending tells, and each zip holds the
// executable, files for the user folder in userdata, and files that go beside the executable.
#ifdef _WIN32
constexpr std::string_view ZipEnding = "Windows.zip";
constexpr std::string_view ExecutableEnding = ".exe";
#else
constexpr std::string_view ZipEnding = "Linux.zip";
constexpr std::string_view ExecutableEnding = ".AppImage";
#endif
constexpr std::string_view UserFolder = "userdata/";

// GitHub answers no request without a User-Agent.
const httplib::Headers Headers{{"User-Agent", "GR2port"}};

#ifndef _WIN32
// The files Linux systems keep their certificate authorities in. The TLS library looks in a
// folder of its own, which no system has.
constexpr std::array AuthorityFiles{
    "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // Fedora
    "/etc/ssl/ca-bundle.pem",                            // openSUSE
    "/etc/ssl/cert.pem",                                 // Alpine
};
#endif

// Sets the timeouts and, on Linux, the certificate authorities that vouch for GitHub.
void Prepare(httplib::Client& client) {
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(10, 0);
#ifndef _WIN32
    const auto authorities = std::ranges::find_if(
        AuthorityFiles, [](const char* file) { return std::filesystem::exists(file); });
    if (authorities != AuthorityFiles.end()) {
        client.set_ca_cert_path(*authorities);
    }
#endif
}

// GitHub's answer to a request of its API, or nothing.
std::optional<nlohmann::json> Ask(const std::string& path) {
    httplib::Client client{GitHub};
    Prepare(client);
    const httplib::Result result = client.Get(path, Headers);
    if (!result || result->status != 200) {
        LOG_INFO(Loader, "GR2port: no answer from GitHub to {}: {}", path,
                 result ? std::to_string(result->status) : httplib::to_string(result.error()));
        return std::nullopt;
    }
    nlohmann::json answer = nlohmann::json::parse(result->body, nullptr, false);
    return answer.is_object() ? std::optional{std::move(answer)} : std::nullopt;
}

// The text of a field; empty for a field that is missing or holds none.
std::string Text(const nlohmann::json& object, const char* field) {
    const auto found = object.find(field);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::filesystem::path Beside(const std::filesystem::path& file, const char* ending) {
    std::filesystem::path beside = file;
    beside += ending;
    return beside;
}

std::optional<Release> FindRelease() {
    const std::string_view build{Common::g_scm_rev};
    if (build.empty()) {
        return std::nullopt;
    }
#ifdef _WIN32
    // An update leaves the executable it replaced, which Windows keeps while it runs.
    std::error_code error;
    std::filesystem::remove(Beside(Common::FS::GetExecutableFile(), ".old"), error);
#else
    // A release's executable is an AppImage, which takes the place of another only.
    if (std::getenv("APPIMAGE") == nullptr) {
        return std::nullopt;
    }
#endif
    // GitHub's latest release is the newest that is neither a pre-release nor a draft.
    const std::optional<nlohmann::json> latest = Ask(std::string{Repository} + "/releases/latest");
    if (!latest) {
        return std::nullopt;
    }
    Release release{Text(*latest, "name"), Text(*latest, "body"), {}};
    const auto assets = latest->find("assets");
    if (assets != latest->end()) {
        for (const nlohmann::json& asset : *assets) {
            const std::string name = Text(asset, "name");
            if (!name.ends_with(ZipEnding)) {
                continue;
            }
            // The workflow names a zip after its build's commit: this build's zip is this build.
            if (name.find(build.substr(0, 8)) != std::string::npos) {
                return std::nullopt;
            }
            release.zip = Text(asset, "browser_download_url");
        }
    }
    if (release.zip.empty()) {
        return std::nullopt;
    }
    // The release is ahead when its tag's commit descends from this build's.
    const std::string tag = Text(*latest, "tag_name");
    const std::optional<nlohmann::json> compared =
        Ask(std::string{Repository} + "/compare/" + std::string{build} + "..." + tag);
    if (!compared || Text(*compared, "status") != "ahead") {
        return std::nullopt;
    }
    LOG_INFO(Loader, "GR2port: release {} is ahead of this build", tag);
    return release;
}

size_t Write(void* file, mz_uint64, const void* bytes, size_t size) {
    static_cast<std::ofstream*>(file)->write(static_cast<const char*>(bytes),
                                             static_cast<std::streamsize>(size));
    return size;
}

// Puts the zip's files in place. The new executable takes this one's place last, once every
// file is written.
bool Extract(const std::string& zip) {
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) {
        return false;
    }
    const std::filesystem::path executable = Common::FS::GetExecutableFile();
    const std::filesystem::path fresh = Beside(executable, ".new");
    const std::filesystem::path& user = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    std::error_code error;
    bool written = true;
    bool has_executable = false;
    for (mz_uint i = 0; written && i < mz_zip_reader_get_num_files(&archive); ++i) {
        mz_zip_archive_file_stat entry;
        if (!mz_zip_reader_file_stat(&archive, i, &entry) || entry.m_is_directory) {
            continue;
        }
        const std::string_view name{entry.m_filename};
        const auto path = [](std::string_view text) {
            return std::filesystem::path{std::u8string{text.begin(), text.end()}};
        };
        std::filesystem::path target;
        if (name.ends_with(ExecutableEnding) && name.find('/') == std::string_view::npos) {
            target = fresh;
            has_executable = true;
        } else if (name.starts_with(UserFolder)) {
            target = user / path(name.substr(UserFolder.size()));
        } else {
            target = executable.parent_path() / path(name);
        }
        std::filesystem::create_directories(target.parent_path(), error);
        std::ofstream file{target, std::ios::binary};
        written =
            mz_zip_reader_extract_to_callback(&archive, i, Write, &file, 0) && !file.flush().fail();
    }
    mz_zip_reader_end(&archive);
    if (!written || !has_executable) {
        std::filesystem::remove(fresh, error);
        return false;
    }
#ifdef _WIN32
    // Windows lets the file of a running executable take another name, not other contents.
    const std::filesystem::path old = Beside(executable, ".old");
    std::filesystem::rename(executable, old, error);
    if (!error) {
        std::filesystem::rename(fresh, executable, error);
        if (error) {
            std::error_code undone;
            std::filesystem::rename(old, executable, undone);
        }
    }
#else
    using std::filesystem::perms;
    std::filesystem::permissions(fresh,
                                 perms::owner_all | perms::group_read | perms::group_exec |
                                     perms::others_read | perms::others_exec,
                                 error);
    std::filesystem::rename(fresh, executable, error);
#endif
    if (error) {
        std::filesystem::remove(fresh, error);
        return false;
    }
    return true;
}

bool InstallNow(const Release& release, Download& download) {
    // The zip's address is https://, the host and the path.
    const size_t path = release.zip.find('/', release.zip.find("//") + 2);
    httplib::Client client{release.zip.substr(0, path)};
    Prepare(client);
    // GitHub sends the download on to the server that holds the file.
    client.set_follow_location(true);
    const httplib::Result result =
        client.Get(release.zip.substr(path), Headers, [&download](size_t received, size_t size) {
            download.received = received;
            download.size = size;
            return !download.stopped;
        });
    if (!result || result->status != 200) {
        LOG_ERROR(Loader, "GR2port: the update's download ended: {}",
                  result ? std::to_string(result->status) : httplib::to_string(result.error()));
        return false;
    }
    if (download.stopped || !Extract(result->body)) {
        LOG_ERROR(Loader, "GR2port: the update's files are not in place");
        return false;
    }
    return true;
}

// Runs work on a thread of its own. The future does not wait for it when it goes.
template <typename Work>
auto OnThread(Work work) {
    std::packaged_task<std::invoke_result_t<Work>()> task{std::move(work)};
    auto answer = task.get_future();
    std::thread{std::move(task)}.detach();
    return answer;
}

} // namespace

std::future<std::optional<Release>> Check() {
    return OnThread(FindRelease);
}

std::future<bool> Install(Release release, std::shared_ptr<Download> download) {
    return OnThread([release = std::move(release), download = std::move(download)] {
        return InstallNow(release, *download);
    });
}

} // namespace Gr2Port::Launcher::Updater
