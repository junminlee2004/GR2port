// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <nlohmann/json.hpp>
#include "common/elf_info.h"
#include "common/gravity_rush.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/game_check/game_patches.h"
#include "core/libraries/gr2_online/online_host.h"
#include "core/libraries/np/np_handler/np_handler.h"
#include "core/user_settings.h"
#include "imgui/shadnet_notifications_layer.h"

namespace Libraries::Gr2Online::Host {

// The trophies of this game are uploaded by the emulator's own trophy library, which hands every
// unlock and every sync to these two functions on the session this file reads. Nothing in this
// folder takes part, and nothing in it may take the session away. When either function changes,
// the upload of this game has to be checked again.
using TrophyList = std::vector<std::pair<s32, u64>>;
static_assert(std::is_same_v<decltype(&Np::NpHandler::ReportTrophyUnlock),
                             void (Np::NpHandler::*)(s32, s32, s32, u64)>);
static_assert(std::is_same_v<decltype(&Np::NpHandler::SyncTrophies),
                             void (Np::NpHandler::*)(s32, s32, const TrophyList&,
                                                     std::function<void(const TrophyList&)>)>);

namespace {

// The port the server of the game listens on unless it is told otherwise.
constexpr int DefaultPort = 8443;

// A token that is on its way is asked for in steps of 50 ms, for 2.5 s in all.
constexpr int TokenWaitSteps = 50;
constexpr std::chrono::milliseconds TokenWaitStep{50};

// The identity of the user's latest shadNet sign-in. It outlives the session: the session drops
// with its connection and comes back seconds later, and a request that left in between under the
// local name and without a token would be refused, or filed under the wrong player.
std::mutex identity_mutex;
s32 kept_user{-1};
std::string kept_name;
std::string kept_token;
// A sign-in whose token nobody has waited for yet.
std::mutex token_wait_mutex;
std::atomic<bool> token_due{false};

std::mutex listeners_mutex;
std::vector<std::function<void(s32, bool)>> listeners;
std::once_flag subscribed;

std::string HeaderSafe(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (static_cast<u8>(c) >= 0x20 && c != 0x7f) {
            out.push_back(c);
        }
    }
    return out;
}

void Keep(s32 user_id, std::string name, std::string token) {
    std::scoped_lock lk{identity_mutex};
    if (kept_user != user_id) {
        kept_user = user_id;
        kept_name.clear();
        kept_token.clear();
    }
    if (!name.empty()) {
        kept_name = std::move(name);
    }
    if (!token.empty()) {
        kept_token = std::move(token);
    }
}

// Takes over what the session of the user holds at this moment. Without a session the copy stays
// as it is.
void Refresh(s32 user_id) {
    auto& np = Np::NpHandler::GetInstance();
    if (user_id < 0 || !np.IsPsnSignedIn(user_id)) {
        return;
    }
    const auto online_id = np.GetOnlineId(user_id);
    Keep(user_id, HeaderSafe({online_id.data, strnlen(online_id.data, sizeof(online_id.data))}),
         np.GetBearerToken(user_id));
}

// NpHandler calls this on the thread that changed the state: the thread that initialises the
// libraries for the login at boot, its own worker for a dropped connection and the reconnect,
// and a thread of the game for a login or logout event. It holds the lock of its callback list
// meanwhile and not the one of its sessions, so the session can be read from here.
void OnSessionState(s32 user_id, Np::NpManager::OrbisNpState state) {
    const bool signed_in = state == Np::NpManager::OrbisNpState::SignedIn;
    // The identity is taken over before anyone is told, so a listener that sends a request
    // already has it.
    if (signed_in && user_id == UserId()) {
        token_due = true;
        Refresh(user_id);
    }
    std::vector<std::function<void(s32, bool)>> told;
    {
        std::scoped_lock lk{listeners_mutex};
        told = listeners;
    }
    for (const auto& listener : told) {
        listener(user_id, signed_in);
    }
}

// The launcher of the game keeps the server's port in its "GR2Fork" section, in both files.
void ReadPort(const std::filesystem::path& path, Server& server, bool& have_port) {
    std::ifstream file{path};
    if (!file) {
        return;
    }
    const auto root = nlohmann::json::parse(file, nullptr, false);
    if (!root.is_object() || !root.contains("GR2Fork") || !root.at("GR2Fork").is_object()) {
        return;
    }
    const auto& section = root.at("GR2Fork");
    if (!have_port && section.contains("httpHostOverridePort") &&
        section.at("httpHostOverridePort").is_number_integer()) {
        // A number that is no port counts as not set.
        const int port = section.at("httpHostOverridePort").get<int>();
        if (port > 0 && port <= 65535) {
            server.port = port;
            have_port = true;
        }
    }
}

// GR2port always plays on the same server.
Server LoadServer() {
    Server server{std::string(Gr2Port::GamePatches::GameServerHost), DefaultPort};
    bool have_port = false;
    ReadPort(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.json", server,
             have_port);
    return server;
}

} // Anonymous namespace

bool IsGravityRush2() {
    return Common::IsGravityRush2();
}

std::string_view Serial() {
    return Common::ElfInfo::Instance().GameSerial();
}

s32 UserId() {
    const User* user = UserManagement.GetUserByPlayerIndex(1);
    return user ? user->user_id : -1;
}

bool Connected() {
    return EmulatorSettings.IsConnectedToNetwork() && !GetServer().host.empty();
}

bool SignedIn() {
    const s32 user_id = UserId();
    return user_id >= 0 && Np::NpHandler::GetInstance().IsPsnSignedIn(user_id);
}

bool HasIdentity() {
    const s32 user_id = UserId();
    Refresh(user_id);
    std::scoped_lock lk{identity_mutex};
    return user_id >= 0 && kept_user == user_id && !kept_name.empty();
}

std::string PlayerName() {
    const User* user = UserManagement.GetUserByPlayerIndex(1);
    if (!user) {
        return {};
    }
    Refresh(user->user_id);
    {
        std::scoped_lock lk{identity_mutex};
        if (kept_user == user->user_id && !kept_name.empty()) {
            return kept_name;
        }
    }
    return HeaderSafe(user->user_name);
}

std::string BearerToken() {
    const s32 user_id = UserId();
    if (user_id < 0) {
        return {};
    }
    auto& np = Np::NpHandler::GetInstance();
    const auto on_its_way = [&np, user_id] {
        return np.IsPsnSignedIn(user_id) && np.GetBearerToken(user_id).empty();
    };
    if (on_its_way()) {
        // The shadNet client asks for the token right after the login is accepted, so the first
        // request after a sign-in may be early. One caller waits and the others queue behind
        // it. A server that hands out no token costs this wait once per sign-in.
        std::scoped_lock lk{token_wait_mutex};
        if (token_due.exchange(false)) {
            for (int i = 0; i < TokenWaitSteps && on_its_way(); ++i) {
                std::this_thread::sleep_for(TokenWaitStep);
            }
        }
    }
    Refresh(user_id);
    std::scoped_lock lk{identity_mutex};
    return kept_user == user_id ? kept_token : std::string{};
}

std::vector<std::pair<std::string, std::string>> IdentityHeaders() {
    std::vector<std::pair<std::string, std::string>> headers;
    if (std::string player = PlayerName(); !player.empty()) {
        headers.emplace_back("X-GR2-Player", std::move(player));
    }
    if (std::string token = BearerToken(); !token.empty()) {
        headers.emplace_back("X-GR2-Auth", std::move(token));
    }
    headers.emplace_back("X-GR2-Version", std::to_string(OnlineVersion));
    return headers;
}

const Server& GetServer() {
    static const Server server = LoadServer();
    return server;
}

void OnSignInChange(std::function<void(s32 user_id, bool signed_in)> cb) {
    {
        std::scoped_lock lk{listeners_mutex};
        listeners.push_back(std::move(cb));
    }
    // One subscription serves every listener of this folder.
    std::call_once(subscribed, [] {
        Np::NpHandler::GetInstance().RegisterStateCallback(OnSessionState, nullptr);
    });
}

void Notify(std::string text) {
    ImGui::ShadNetNotify::Push(ImGui::ShadNetNotify::Kind::Info, std::move(text));
}

} // namespace Libraries::Gr2Online::Host
