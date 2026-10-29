#pragma once

// the console's side of ponte: saved macs, finding them on the lan, the http
// calls, and what this switch holds. the wire format lives in ponte_proto.hpp.

#include "utils/ponte_proto.hpp"

#include <string>
#include <vector>
#include <stop_token>
#include <switch.h>

namespace sphaira::ponte {

// a paired (or about to be paired) mac, kept in /config/sphaira/ponte.ini under
// its server id - the same id discovery reports, so a mac that changed address
// is still recognised.
struct Server {
    std::string id{};
    std::string name{};
    std::string address{}; // host:port, as last seen.
    std::string token{};   // empty until paired, and for a mac that doesn't pair.
};

void LoadServers(std::vector<Server>& out);
void SaveServer(const Server& server);
void DeleteServer(const std::string& id);

// a mac that answered PONTE?1 (SPEC §3).
struct Found {
    std::string id{};
    std::string name{};
    std::string address{}; // the reply's sender and the port it named.
    int protocol{};
};

// broadcasts on the lan and collects replies for about a second. blocking: run
// it from a worker thread.
auto Discover(std::stop_token token) -> std::vector<Found>;

// what went wrong with a request, phrased for the person holding the console.
struct Failure {
    bool reached{};       // the mac answered, if with an error.
    long status{};        // http status, 0 when nothing answered.
    std::string code{};   // the SPEC §6 code, when the mac sent one.
    std::string message{};
};

// every call is blocking, for a worker thread, and stops early on `token`.
struct Client {
    explicit Client(const Server& server);

    auto Info(ServerInfo& out, Failure& fail, std::stop_token token) const -> bool;
    // fills `server.token` on success. the mac's code changes after every pairing.
    auto Pair(const std::string& code, Server& server, Failure& fail, std::stop_token token) const -> bool;
    // tells the mac what the console holds and gets the catalog back with the
    // console's state on every item (SPEC §5.7).
    auto PostState(const ConsoleState& state, Catalog& out, Failure& fail, std::stop_token token) const -> bool;
    auto Item(const std::string& id, ItemDetail& out, Failure& fail, std::stop_token token) const -> bool;
    // cheap: whether anything was added since `since` (SPEC §5.9).
    auto GetChanges(const std::string& since, Changes& out, Failure& fail, std::stop_token token) const -> bool;
    // best effort: a report that doesn't arrive changes nothing on the console.
    auto SendReport(const std::string& id, const Report& report, std::stop_token token) const -> bool;

    // what an install streams from: the file's url, and the headers to send with
    // every range request (auth, protocol, the revision it must still be).
    auto FileUrl(const std::string& id, std::int64_t index) const -> std::string;
    auto FileHeaders(const std::string& revision) const -> std::vector<std::string>;
    // empty when the item has no icon.
    auto IconUrl(const CatalogItem& item) const -> std::string;
    auto IconHeaders() const -> std::vector<std::pair<std::string, std::string>>;

    auto BaseUrl() const -> const std::string& { return m_base_url; }

private:
    auto Get(const std::string& route, std::string& body, Failure& fail, std::stop_token token) const -> bool;
    // `deadline_ms` > 0 gives up after that long, connect included: curl has no
    // timeout of its own here, and a mac that went away would hold it minutes.
    auto Post(const std::string& route, const std::string& json, std::string& body, Failure& fail, std::stop_token token, u64 deadline_ms = 0) const -> bool;

    std::string m_base_url{};
    std::string m_token{};
};

// what this console holds and has room for, for POST /console/state. opens ns
// for itself; call from a worker.
auto ScanConsole() -> ConsoleState;

// the console's own nickname, what the mac lists it as.
auto ConsoleName() -> std::string;

} // namespace sphaira::ponte
