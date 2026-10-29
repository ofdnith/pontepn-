#include "utils/ponte.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "download.hpp"
#include "fs.hpp"
#include "i18n.hpp"
#include "log.hpp"
#include "title_info.hpp"
#include "yati/nx/ns.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <minIni.h>
#include <switch.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sphaira::ponte {
namespace {

#ifdef PONTE_STANDALONE
#define PONTE_CONFIG_DIR "/config/ponte/"
#else
#define PONTE_CONFIG_DIR "/config/sphaira/"
#endif
#define PONTE_INI_PATH PONTE_CONFIG_DIR "ponte.ini"

constexpr s64 DISCOVERY_TIMEOUT_MS = 1200;
constexpr s64 DISCOVERY_RESEND_MS = 400;

struct SocketWrapper {
    SocketWrapper(int af, int type, int proto) : sock{socket(af, type, proto)} {}
    ~SocketWrapper() {
        if (sock >= 0) {
            close(sock);
        }
    }
    operator int() const { return sock; }
    int sock{};
};

// the console accepts a send to the limited broadcast and then drops it, having
// no route; the subnet-directed one is what carries. both go out, as ownfoil's
// discovery found that which one works depends on the router.
auto BroadcastTargets() -> std::vector<in_addr_t> {
    std::vector<in_addr_t> targets{};

    u32 addr, mask, gateway, primary_dns, secondary_dns;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&addr, &mask, &gateway, &primary_dns, &secondary_dns)) && addr && mask) {
        targets.emplace_back(addr | ~mask);
    }

    targets.emplace_back(htonl(INADDR_BROADCAST));
    return targets;
}

auto Describe(long status, const ApiError& api) -> std::string {
    if (api.code == "protocol_mismatch" || status == 426) {
        return "This Mac runs a different version of Ponte. Update the Mac app and this one."_i18n;
    }
    if (api.code == "too_many_attempts" || status == 429) {
        return "Too many wrong codes. Wait a minute and try again."_i18n;
    }
    if (api.code == "unauthorized" || status == 401) {
        return "This console is not paired with the Mac."_i18n;
    }
    if (api.code == "item_not_ready" || status == 409) {
        return "The Mac is still preparing this item."_i18n;
    }
    if (api.code == "revision_changed" || status == 412) {
        return "The item changed on the Mac. Refresh and try again."_i18n;
    }
    if (api.code == "not_found" || status == 404) {
        return "The Mac no longer has this item. Refresh the list."_i18n;
    }
    if (!api.message.empty()) {
        return api.message;
    }
    return "The Mac returned an error"_i18n + " (" + std::to_string(status) + ")";
}

auto Firmware() -> std::string {
    const auto v = hosversionGet();
    return FormatFirmware(HOSVER_MAJOR(v), HOSVER_MINOR(v));
}

auto StorageName(u8 storage_id) -> const char* {
    switch (storage_id) {
        case NcmStorageId_SdCard: return "sd";
        default: return "nand";
    }
}

} // namespace

void LoadServers(std::vector<Server>& out) {
    static const auto cb = [](const mTCHAR* section, const mTCHAR* key, const mTCHAR* value, void* user) -> int {
        auto e = static_cast<std::vector<Server>*>(user);
        if (!section || !key || !value) {
            return 1;
        }

        if (e->empty() || std::strcmp(section, e->back().id.c_str())) {
            e->emplace_back();
            e->back().id = section;
        }

        if (!std::strcmp(key, "name")) {
            e->back().name = value;
        } else if (!std::strcmp(key, "address")) {
            e->back().address = value;
        } else if (!std::strcmp(key, "token")) {
            e->back().token = value;
        }
        return 1;
    };

    out.clear();
    ini_browse(cb, &out, PONTE_INI_PATH);

    // an entry with no address can't be reached and was never written whole.
    std::erase_if(out, [](const auto& e) { return e.address.empty(); });
    log_write("[PONTE] %zu saved mac(s)\n", out.size());
}

void SaveServer(const Server& server) {
    if (server.id.empty()) {
        return;
    }

    fs::FsNativeSd().CreateDirectoryRecursively(PONTE_CONFIG_DIR);
    ini_puts(server.id.c_str(), "name", server.name.c_str(), PONTE_INI_PATH);
    ini_puts(server.id.c_str(), "address", server.address.c_str(), PONTE_INI_PATH);
    ini_puts(server.id.c_str(), "token", server.token.c_str(), PONTE_INI_PATH);
}

void DeleteServer(const std::string& id) {
    ini_puts(id.c_str(), nullptr, nullptr, PONTE_INI_PATH);
}

auto Discover(std::stop_token token) -> std::vector<Found> {
    std::vector<Found> out{};

    SocketWrapper sock{AF_INET, SOCK_DGRAM, 0};
    if (sock < 0) {
        log_write("[PONTE] discovery: socket: %s\n", strerror(errno));
        return out;
    }

    int enable = 1;
    if (0 > setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable))) {
        log_write("[PONTE] discovery: broadcast: %s\n", strerror(errno));
        return out;
    }

    const auto targets = BroadcastTargets();
    const auto ask = [&]() {
        for (const auto target : targets) {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(DISCOVERY_PORT);
            dest.sin_addr.s_addr = target;
            sendto(sock, DISCOVERY_QUERY, std::strlen(DISCOVERY_QUERY), 0, (const sockaddr*)&dest, sizeof(dest));
        }
    };

    ask();
    const auto start_ns = armTicksToNs(armGetSystemTick());
    s64 next_send_ms = DISCOVERY_RESEND_MS;
    char buf[1024];

    while (!token.stop_requested()) {
        const s64 elapsed_ms = (armTicksToNs(armGetSystemTick()) - start_ns) / 1'000'000;
        const s64 remaining_ms = DISCOVERY_TIMEOUT_MS - elapsed_ms;
        if (remaining_ms <= 0) {
            break;
        }

        // a single lost datagram would mean an empty search.
        if (elapsed_ms >= next_send_ms) {
            ask();
            next_send_ms += DISCOVERY_RESEND_MS;
        }

        pollfd pfd{ .fd = sock, .events = POLLIN };
        const auto rc = poll(&pfd, 1, static_cast<int>(std::min(remaining_ms, next_send_ms - elapsed_ms)));
        if (rc < 0) {
            break;
        } else if (rc == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }

        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const auto len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &from_len);
        if (len <= 0) {
            continue;
        }

        DiscoveryReply reply{};
        if (!ParseDiscoveryReply(std::string_view{buf, static_cast<size_t>(len)}, reply)) {
            continue;
        }

        // the host is where the reply came from (SPEC §3).
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

        Found found{};
        found.id = reply.id;
        found.name = reply.name;
        found.protocol = reply.protocol;
        found.address = std::string{ip} + ":" + std::to_string(reply.port);

        const auto it = std::find_if(out.begin(), out.end(), [&](const auto& e) { return e.id == found.id; });
        if (it == out.end()) {
            out.emplace_back(std::move(found));
        } else {
            *it = std::move(found);
        }
    }

    log_write("[PONTE] discovery: %zu mac(s)\n", out.size());
    return out;
}

Client::Client(const Server& server)
: m_base_url{ponte::BaseUrl(server.address)}
, m_token{server.token} {
}

auto Client::Get(const std::string& route, std::string& body, Failure& fail, std::stop_token token) const -> bool {
    return Post(route, {}, body, fail, token);
}

// a single path for both: curl posts when there are fields, and gets otherwise.
auto Client::Post(const std::string& route, const std::string& json, std::string& body, Failure& fail, std::stop_token token, u64 deadline_ms) const -> bool {
    fail = {};
    if (m_base_url.empty()) {
        fail.message = "No address for this Mac"_i18n;
        return false;
    }

    curl::Header header{};
    header.m_map["X-Ponte-Protocol"] = std::to_string(PROTOCOL_VERSION);
    if (!m_token.empty()) {
        header.m_map["Authorization"] = "Bearer " + m_token;
    }
    if (!json.empty()) {
        header.m_map["Content-Type"] = "application/json";
    }

    // curl calls this about once a second, connecting or not.
    const auto started = armGetSystemTick();
    curl::OnProgress on_progress = [started, deadline_ms](s64, s64, s64, s64) -> bool {
        return !deadline_ms || armTicksToNs(armGetSystemTick() - started) / 1'000'000 < deadline_ms;
    };

    const auto result = curl::Api().ToMemory(
        curl::Url{m_base_url + route},
        curl::Fields{json},
        std::move(header),
        std::move(on_progress),
        // the error body is the SPEC §6 document, which says what went wrong.
        curl::Flags{curl::Flag_KeepErrorBody},
        curl::StopToken{token}
    );

    if (token.stop_requested()) {
        fail.message = "Cancelled"_i18n;
        return false;
    }

    if (!result.success || !result.code) {
        fail.message = "Could not reach the Mac. Is the Ponte app open on it?"_i18n;
        return false;
    }

    fail.reached = true;
    fail.status = result.code;
    body.assign(reinterpret_cast<const char*>(result.data.data()), result.data.size());

    if (result.code < 200 || result.code >= 300) {
        ApiError api{};
        ParseError(body, api);
        fail.code = api.code;
        fail.message = Describe(result.code, api);
        log_write("[PONTE] %s -> %ld %s\n", route.c_str(), result.code, api.code.c_str());
        return false;
    }

    return true;
}

auto Client::Info(ServerInfo& out, Failure& fail, std::stop_token token) const -> bool {
    std::string body;
    if (!Get(RouteInfo(), body, fail, token)) {
        return false;
    }
    if (!ParseInfo(body, out)) {
        fail.message = "This doesn't look like a Ponte server"_i18n;
        return false;
    }
    if (out.protocol != PROTOCOL_VERSION) {
        fail.message = Describe(426, {});
        return false;
    }
    return true;
}

auto Client::Pair(const std::string& code, Server& server, Failure& fail, std::stop_token token) const -> bool {
    const auto json = BuildPairBody(code, ConsoleName(), APP_VERSION, Firmware());

    std::string body;
    if (!Post(RoutePair(), json, body, fail, token)) {
        // a wrong code is also a 401, but it means something else here.
        if (fail.status == 401) {
            fail.message = "Wrong code. Check the code shown on the Mac."_i18n;
        }
        return false;
    }

    std::string client_token;
    if (!ParsePairReply(body, client_token)) {
        fail.message = "This doesn't look like a Ponte server"_i18n;
        return false;
    }

    server.token = client_token;
    return true;
}

auto Client::PostState(const ConsoleState& state, Catalog& out, Failure& fail, std::stop_token token) const -> bool {
    std::string body;
    if (!Post(RouteConsoleState(), BuildConsoleState(state), body, fail, token)) {
        return false;
    }
    if (!ParseCatalog(body, out)) {
        fail.message = "The Mac sent a catalog this app can't read"_i18n;
        return false;
    }

    // the mac answers for the platform the token was paired as; anything else
    // in the list isn't something this console can install.
    std::erase_if(out.items, [](const auto& e) { return !e.platform.empty() && e.platform != "switch"; });
    return true;
}

auto Client::Item(const std::string& id, ItemDetail& out, Failure& fail, std::stop_token token) const -> bool {
    std::string body;
    if (!Get(RouteItem(id), body, fail, token)) {
        return false;
    }
    if (!ParseItemDetail(body, out)) {
        fail.message = "The Mac sent an item this app can't read"_i18n;
        return false;
    }
    return true;
}

auto Client::GetChanges(const std::string& since, Changes& out, Failure& fail, std::stop_token token) const -> bool {
    std::string body;
    if (!Get(RouteChanges(curl::EscapeString(since)), body, fail, token)) {
        return false;
    }
    if (!ParseChanges(body, out)) {
        fail.message = "The Mac sent an answer this app can't read"_i18n;
        return false;
    }
    return true;
}

auto Client::SendReport(const std::string& id, const Report& report, std::stop_token token) const -> bool {
    std::string body;
    Failure fail{};
    // sent even after a cancel, from the thread the progress box joins on the
    // main thread: it must not wait long on a mac that has gone.
    return Post(RouteReport(id), BuildReport(report), body, fail, token, 5000);
}

auto Client::FileUrl(const std::string& id, std::int64_t index) const -> std::string {
    return m_base_url + RouteFile(id, index);
}

auto Client::FileHeaders(const std::string& revision) const -> std::vector<std::string> {
    std::vector<std::string> out;
    out.emplace_back("X-Ponte-Protocol: " + std::to_string(PROTOCOL_VERSION));
    if (!m_token.empty()) {
        out.emplace_back("Authorization: Bearer " + m_token);
    }
    // every range request carries it, so a file that changes on the mac in the
    // middle of an install fails with 412 rather than mixing two versions.
    if (!revision.empty()) {
        out.emplace_back("X-Ponte-Revision: " + revision);
    }
    return out;
}

auto Client::IconUrl(const CatalogItem& item) const -> std::string {
    // the catalog gives a path under the server root (SPEC §5.3).
    if (item.icon.empty() || item.icon.front() != '/' || m_base_url.empty()) {
        return {};
    }
    return m_base_url + item.icon;
}

auto Client::IconHeaders() const -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    out.emplace_back("X-Ponte-Protocol", std::to_string(PROTOCOL_VERSION));
    if (!m_token.empty()) {
        out.emplace_back("Authorization", "Bearer " + m_token);
    }
    return out;
}

auto ScanConsole() -> ConsoleState {
    ConsoleState out{};
    out.platform = "switch";
    out.firmware = Firmware();

    s64 free{}, total{};
    if (R_SUCCEEDED(App::GetEmmcSize(&free, &total))) {
        out.storages.push_back({"nand", "System memory"_i18n, free, total});
    }
    if (R_SUCCEEDED(App::GetSdSize(&free, &total))) {
        out.storages.push_back({"sd", "microSD card"_i18n, free, total});
    }

    // ns is only open while the games or saves menu exists, so this opens it
    // for itself, as ownfoil's scan does.
    if (R_FAILED(ns::Initialize())) {
        log_write("[PONTE] failed to open ns\n");
        return out;
    }
    ON_SCOPE_EXIT(ns::Exit());

    title::ForEachApplicationRecord([&](std::span<const NsApplicationRecord> records) {
        for (const auto& record : records) {
            title::MetaEntries metas;
            const auto flags = title::ContentFlag_Application | title::ContentFlag_Patch | title::ContentFlag_AddOnContent;
            if (R_FAILED(title::GetMetaEntries(record.application_id, metas, flags))) {
                continue;
            }

            for (const auto& meta : metas) {
                // a game card isn't something the console holds once it's out.
                if (meta.storageID != NcmStorageId_SdCard && meta.storageID != NcmStorageId_BuiltInUser) {
                    continue;
                }

                Installed e{};
                auto id = meta.application_id;
                switch (meta.meta_type) {
                    case NcmContentMetaType_Application:
                        e.content = Content::Base;
                        break;
                    case NcmContentMetaType_Patch:
                        e.content = Content::Update;
                        // the mac keys an update by its own id, ending in 800.
                        if (ContentOfTitleId(id) != Content::Update) {
                            id = (id & ~0xFFFULL) | 0x800;
                        }
                        break;
                    case NcmContentMetaType_AddOnContent:
                        e.content = Content::Dlc;
                        break;
                    default:
                        continue;
                }

                e.title_id = FormatTitleId(id);
                e.version = std::to_string(meta.version);
                e.storage = StorageName(meta.storageID);
                out.installed.emplace_back(std::move(e));
            }
        }
    });

    log_write("[PONTE] console holds %zu content entries\n", out.installed.size());
    return out;
}

auto ConsoleName() -> std::string {
    std::string out{"Nintendo Switch"};
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysDeviceNickName nickname{};
        if (R_SUCCEEDED(setsysGetDeviceNickname(&nickname)) && nickname.nickname[0]) {
            out = nickname.nickname;
        }
        setsysExit();
    }
    return out;
}

} // namespace sphaira::ponte
