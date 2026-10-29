#pragma once

// the ponte protocol (v1), as the switch speaks it: what goes over the wire and
// what the console makes of it. nothing in here touches libnx, curl or the ui,
// so it builds and is tested on a pc against the reference server in pontepn-.
// protocol/SPEC.md in that repository is the source of truth.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sphaira::ponte {

inline constexpr int PROTOCOL_VERSION = 1;
inline constexpr std::uint16_t DEFAULT_PORT = 47101;
inline constexpr std::uint16_t DISCOVERY_PORT = 47100;
inline constexpr char DISCOVERY_QUERY[] = "PONTE?1";
inline constexpr char API_PREFIX[] = "/api/v1";

// SPEC §8 rule 2: the size of what is copied, plus a 5% margin.
inline constexpr std::int64_t SPACE_MARGIN_PERCENT = 5;

// one reply to the PONTE?1 broadcast (SPEC §3). the host is whoever sent it,
// never anything the server claims about itself.
struct DiscoveryReply {
    int protocol{};
    std::string name{};
    std::string id{};
    std::uint16_t port{};
};

// GET /info (SPEC §5.1).
struct ServerInfo {
    int protocol{};
    std::string server{};
    std::string name{};
    std::string id{};
    bool pairing_required{true};
};

// the body of every error response (SPEC §6).
struct ApiError {
    std::string code{};
    std::string message{};
};

enum class Content { Base, Update, Dlc, Unknown };

// SPEC §5.7. None is a catalog that came from GET /catalog, which says nothing
// about the console.
enum class State { None, New, Installed, UpdateAvailable, Incompatible };

struct CatalogItem {
    std::string id{};
    std::string revision{};
    std::string platform{};
    std::string kind{}; // nsp, nsz, xci, xcz on the switch.
    Content content{Content::Unknown};
    std::string title{};
    std::string title_id{};         // empty when the mac doesn't know it.
    std::string version{};          // the switch counts these as an integer, sent as text.
    std::string required_firmware{};
    std::int64_t size{};
    std::int64_t file_count{};
    std::string parent_title_id{};
    std::vector<std::string> warnings{};
    std::string added_at{};
    std::string icon{};             // a path under the server root, empty when there is none.
    State state{State::None};
    std::string state_reason{};
};

struct Catalog {
    std::string generated_at{};
    std::vector<CatalogItem> items{};
};

struct ItemFile {
    std::int64_t index{};
    std::string path{};
    std::int64_t size{};
    std::string sha256{}; // empty while the mac is still hashing.
};

// GET /items/{id} (SPEC §5.4).
struct ItemDetail {
    CatalogItem item{};
    std::vector<ItemFile> files{};
};

struct Storage {
    std::string id{};    // nand or sd.
    std::string label{};
    std::int64_t free{};
    std::int64_t total{};
};

struct Installed {
    std::string title_id{}; // the content's own id: an update's ends in 800.
    std::string version{};
    Content content{Content::Base};
    std::string storage{};
};

// POST /console/state (SPEC §5.7).
struct ConsoleState {
    std::string platform{"switch"};
    std::string firmware{};
    std::vector<Storage> storages{};
    std::vector<Installed> installed{};
};

enum class ReportResult { Installed, Copied, Failed, Cancelled };

// POST /items/{id}/report (SPEC §5.8).
struct Report {
    ReportResult result{ReportResult::Failed};
    std::string storage{};
    std::string detail{};
    std::int64_t duration_s{};
};

auto ContentName(Content content) -> const char*;
auto ParseContent(std::string_view text) -> Content;
auto StateName(State state) -> const char*;
auto ParseState(std::string_view text) -> State;
auto ReportResultName(ReportResult result) -> const char*;

// all of these return false on anything that isn't the shape the SPEC promises.
auto ParseDiscoveryReply(std::string_view json, DiscoveryReply& out) -> bool;
auto ParseInfo(std::string_view json, ServerInfo& out) -> bool;
// false when the body isn't an error document; `out` is untouched then.
auto ParseError(std::string_view json, ApiError& out) -> bool;
auto ParsePairReply(std::string_view json, std::string& token) -> bool;
// GET /catalog and POST /console/state answer with the same document, the
// latter with `state` and `state_reason` filled in.
auto ParseCatalog(std::string_view json, Catalog& out) -> bool;
auto ParseItemDetail(std::string_view json, ItemDetail& out) -> bool;

// GET /catalog/changes (SPEC §5.9).
struct Changes {
    bool changed{};
    std::int64_t new_items{};
    std::string generated_at{};
};
auto ParseChanges(std::string_view json, Changes& out) -> bool;

auto BuildPairBody(const std::string& code, const std::string& name, const std::string& version, const std::string& firmware) -> std::string;
auto BuildConsoleState(const ConsoleState& state) -> std::string;
auto BuildReport(const Report& report) -> std::string;

// SPEC §5.4: relative, '/' separated, no "..", no leading '/', no empty parts.
auto IsSafePath(std::string_view path) -> bool;

// 16 uppercase hex digits, the way the mac spells title ids.
auto FormatTitleId(std::uint64_t id) -> std::string;
auto ParseTitleId(std::string_view text, std::uint64_t& out) -> bool;
// SPEC §1.2, from the title id alone.
auto ContentOfTitleId(std::uint64_t id) -> Content;

// "X.YY", the only firmware spelling the SPEC allows.
auto FormatFirmware(int major, int minor) -> std::string;

// whether `size` plus the margin fits in `free`.
auto Fits(std::int64_t size, std::int64_t free) -> bool;

// one line of the list the console draws: the catalog grouped by game, a base
// game first and its updates and add-ons under it (SPEC §8 rule 1), games in
// the catalog's own order (by title).
struct Row {
    std::size_t item{};   // index into the catalog's items.
    int depth{};          // 0 a game, or something whose game isn't in the catalog; 1 under it.
    std::string group{};  // the base title id the row belongs to, or the item id.
};

auto GroupRows(const std::vector<CatalogItem>& items) -> std::vector<Row>;

// the order the chosen items install in: grouped as above, so within one game
// it is always base → update → dlc (SPEC §8 rule 8).
auto InstallOrder(const std::vector<CatalogItem>& items, const std::vector<std::size_t>& chosen) -> std::vector<std::size_t>;

// what a new install of these would sensibly come with: the item itself, and
// for a base game the newest update and every dlc the catalog holds that the
// console doesn't already have. used to pre-select a game's rows.
auto Companions(const std::vector<CatalogItem>& items, std::size_t base) -> std::vector<std::size_t>;

// the file name yati is handed: it picks the container by extension.
auto InstallPathFor(const CatalogItem& item) -> std::string;

// route builders, relative to the server root ("http://host:port").
auto RouteInfo() -> std::string;
auto RoutePair() -> std::string;
auto RouteCatalog(std::string_view platform) -> std::string;
auto RouteConsoleState() -> std::string;
auto RouteItem(std::string_view id) -> std::string;
auto RouteFile(std::string_view id, std::int64_t index) -> std::string;
auto RouteReport(std::string_view id) -> std::string;
auto RouteChanges(std::string_view since) -> std::string;

// "host" or "host:port" → "http://host:port", empty when it can't be one.
auto BaseUrl(std::string_view address) -> std::string;

} // namespace sphaira::ponte
