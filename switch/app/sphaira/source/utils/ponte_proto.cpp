#include "utils/ponte_proto.hpp"

#include <yyjson.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace sphaira::ponte {
namespace {

struct DocDeleter {
    void operator()(yyjson_doc* doc) const { yyjson_doc_free(doc); }
};
struct MutDocDeleter {
    void operator()(yyjson_mut_doc* doc) const { yyjson_mut_doc_free(doc); }
};
using Doc = std::unique_ptr<yyjson_doc, DocDeleter>;
using MutDoc = std::unique_ptr<yyjson_mut_doc, MutDocDeleter>;

auto Read(std::string_view json) -> Doc {
    if (json.empty()) {
        return {};
    }
    // yyjson only reads, but wants a mutable pointer in older releases.
    return Doc{yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG)};
}

auto RootObject(const Doc& doc) -> yyjson_val* {
    if (!doc) {
        return nullptr;
    }
    const auto root = yyjson_doc_get_root(doc.get());
    return root && yyjson_is_obj(root) ? root : nullptr;
}

// a missing key and a json null both read as empty: the SPEC allows null for
// every optional string, and the console treats both as "not known".
auto GetStr(yyjson_val* obj, const char* key) -> std::string {
    const auto v = yyjson_obj_get(obj, key);
    return v && yyjson_is_str(v) ? std::string{yyjson_get_str(v), yyjson_get_len(v)} : std::string{};
}

auto GetInt(yyjson_val* obj, const char* key, std::int64_t def = 0) -> std::int64_t {
    const auto v = yyjson_obj_get(obj, key);
    if (!v) {
        return def;
    }
    if (yyjson_is_sint(v)) {
        return yyjson_get_sint(v);
    }
    if (yyjson_is_uint(v)) {
        // sizes are 64 bit (SPEC §2); nothing real comes near the top bit.
        return static_cast<std::int64_t>(yyjson_get_uint(v));
    }
    return def;
}

auto GetBool(yyjson_val* obj, const char* key, bool def) -> bool {
    const auto v = yyjson_obj_get(obj, key);
    return v && yyjson_is_bool(v) ? yyjson_get_bool(v) : def;
}

auto ParseItem(yyjson_val* obj, CatalogItem& out) -> bool {
    if (!obj || !yyjson_is_obj(obj)) {
        return false;
    }

    out.id = GetStr(obj, "id");
    // the id is the only thing every later request is keyed on.
    if (out.id.size() != 16) {
        return false;
    }

    out.revision = GetStr(obj, "revision");
    out.platform = GetStr(obj, "platform");
    out.kind = GetStr(obj, "kind");
    out.content = ParseContent(GetStr(obj, "content"));
    out.title = GetStr(obj, "title");
    out.title_id = GetStr(obj, "title_id");
    out.version = GetStr(obj, "version");
    out.required_firmware = GetStr(obj, "required_firmware");
    out.size = GetInt(obj, "size");
    out.file_count = GetInt(obj, "file_count");
    out.parent_title_id = GetStr(obj, "parent_title_id");
    out.added_at = GetStr(obj, "added_at");
    out.icon = GetStr(obj, "icon");
    out.state = ParseState(GetStr(obj, "state"));
    out.state_reason = GetStr(obj, "state_reason");

    out.warnings.clear();
    if (const auto arr = yyjson_obj_get(obj, "warnings"); arr && yyjson_is_arr(arr)) {
        size_t idx, max;
        yyjson_val* w;
        yyjson_arr_foreach(arr, idx, max, w) {
            if (yyjson_is_str(w)) {
                out.warnings.emplace_back(yyjson_get_str(w));
            }
        }
    }

    // SPEC §5.3: title is never empty. a server that breaks that still gets a
    // name on screen.
    if (out.title.empty()) {
        out.title = out.title_id.empty() ? out.id : out.title_id;
    }

    // ids are compared against what ncm reports, which this code spells upper.
    std::transform(out.title_id.begin(), out.title_id.end(), out.title_id.begin(), ::toupper);
    std::transform(out.parent_title_id.begin(), out.parent_title_id.end(), out.parent_title_id.begin(), ::toupper);
    return true;
}

auto Write(const MutDoc& doc) -> std::string {
    size_t len{};
    char* json = yyjson_mut_write(doc.get(), YYJSON_WRITE_NOFLAG, &len);
    if (!json) {
        return {};
    }
    std::string out{json, len};
    std::free(json);
    return out;
}

auto ContentRank(Content content) -> int {
    switch (content) {
        case Content::Base: return 0;
        case Content::Update: return 1;
        case Content::Dlc: return 2;
        case Content::Unknown: return 3;
    }
    return 3;
}

auto VersionNumber(const std::string& version) -> std::uint64_t {
    if (version.empty()) {
        return 0;
    }
    return std::strtoull(version.c_str(), nullptr, 10);
}

// which game a row belongs to: a base game is its own, an update or dlc the one
// the mac named as its parent. without a title id an item is a game of its own.
auto GroupKey(const CatalogItem& item) -> std::string {
    if (item.content != Content::Base && !item.parent_title_id.empty()) {
        return item.parent_title_id;
    }
    if (!item.title_id.empty()) {
        return item.title_id;
    }
    return item.id;
}

} // namespace

auto ContentName(Content content) -> const char* {
    switch (content) {
        case Content::Base: return "base";
        case Content::Update: return "update";
        case Content::Dlc: return "dlc";
        case Content::Unknown: return "unknown";
    }
    return "unknown";
}

auto ParseContent(std::string_view text) -> Content {
    if (text == "base") return Content::Base;
    if (text == "update") return Content::Update;
    if (text == "dlc") return Content::Dlc;
    return Content::Unknown;
}

auto StateName(State state) -> const char* {
    switch (state) {
        case State::None: return "";
        case State::New: return "new";
        case State::Installed: return "installed";
        case State::UpdateAvailable: return "update_available";
        case State::Incompatible: return "incompatible";
    }
    return "";
}

auto ParseState(std::string_view text) -> State {
    if (text == "new") return State::New;
    if (text == "installed") return State::Installed;
    if (text == "update_available") return State::UpdateAvailable;
    if (text == "incompatible") return State::Incompatible;
    return State::None;
}

auto ReportResultName(ReportResult result) -> const char* {
    switch (result) {
        case ReportResult::Installed: return "installed";
        case ReportResult::Copied: return "copied";
        case ReportResult::Failed: return "failed";
        case ReportResult::Cancelled: return "cancelled";
    }
    return "failed";
}

auto ParseDiscoveryReply(std::string_view json, DiscoveryReply& out) -> bool {
    const auto doc = Read(json);
    const auto root = RootObject(doc);
    if (!root) {
        return false;
    }

    const auto protocol = GetInt(root, "ponte", -1);
    const auto port = GetInt(root, "port", -1);
    const auto id = GetStr(root, "id");
    if (protocol < 1 || port <= 0 || port > 0xFFFF || id.empty()) {
        return false;
    }

    out.protocol = static_cast<int>(protocol);
    out.port = static_cast<std::uint16_t>(port);
    out.id = id;
    out.name = GetStr(root, "name");
    if (out.name.empty()) {
        out.name = id;
    }
    return true;
}

auto ParseInfo(std::string_view json, ServerInfo& out) -> bool {
    const auto doc = Read(json);
    const auto root = RootObject(doc);
    if (!root) {
        return false;
    }

    out.protocol = static_cast<int>(GetInt(root, "protocol", -1));
    out.server = GetStr(root, "server");
    out.name = GetStr(root, "name");
    out.id = GetStr(root, "id");
    out.pairing_required = GetBool(root, "pairing_required", true);
    return out.protocol > 0 && !out.id.empty();
}

auto ParseError(std::string_view json, ApiError& out) -> bool {
    const auto doc = Read(json);
    const auto root = RootObject(doc);
    if (!root) {
        return false;
    }

    const auto err = yyjson_obj_get(root, "error");
    if (!err || !yyjson_is_obj(err)) {
        return false;
    }

    out.code = GetStr(err, "code");
    out.message = GetStr(err, "message");
    return !out.code.empty();
}

auto ParsePairReply(std::string_view json, std::string& token) -> bool {
    const auto doc = Read(json);
    const auto root = RootObject(doc);
    if (!root) {
        return false;
    }

    const auto v = yyjson_obj_get(root, "client_token");
    if (!v || !yyjson_is_str(v)) {
        return false;
    }

    // empty is a server with pairing switched off (SPEC §4).
    token = yyjson_get_str(v);
    return true;
}

auto ParseCatalog(std::string_view json, Catalog& out) -> bool {
    const auto doc = Read(json);
    const auto root = RootObject(doc);
    if (!root) {
        return false;
    }

    const auto items = yyjson_obj_get(root, "items");
    if (!items || !yyjson_is_arr(items)) {
        return false;
    }

    out.generated_at = GetStr(root, "generated_at");
    out.items.clear();
    out.items.reserve(yyjson_arr_size(items));

    size_t idx, max;
    yyjson_val* v;
    yyjson_arr_foreach(items, idx, max, v) {
        CatalogItem item{};
        // one malformed entry costs that entry, not the whole catalog.
        if (ParseItem(v, item)) {
            out.items.emplace_back(std::move(item));
        }
    }

    return true;
}

auto ParseItemDetail(std::string_view json, ItemDetail& out) -> bool {
    const auto doc = Read(json);
    const auto root = RootObject(doc);
    if (!root || !ParseItem(root, out.item)) {
        return false;
    }

    const auto files = yyjson_obj_get(root, "files");
    if (!files || !yyjson_is_arr(files)) {
        return false;
    }

    out.files.clear();
    size_t idx, max;
    yyjson_val* v;
    yyjson_arr_foreach(files, idx, max, v) {
        if (!yyjson_is_obj(v)) {
            return false;
        }

        ItemFile file{};
        file.index = GetInt(v, "index", -1);
        file.path = GetStr(v, "path");
        file.size = GetInt(v, "size", -1);
        file.sha256 = GetStr(v, "sha256");

        // the client rejects any path outside the rules (SPEC §5.4), and the
        // whole item with it: a server sending one is not to be trusted.
        if (file.index < 0 || file.size < 0 || !IsSafePath(file.path)) {
            return false;
        }

        out.files.emplace_back(std::move(file));
    }

    return true;
}

auto ParseChanges(std::string_view json, Changes& out) -> bool {
    const auto doc = Read(json);
    const auto root = RootObject(doc);
    if (!root) {
        return false;
    }

    const auto changed = yyjson_obj_get(root, "changed");
    if (!changed || !yyjson_is_bool(changed)) {
        return false;
    }

    out.changed = yyjson_get_bool(changed);
    out.new_items = GetInt(root, "new_items");
    out.generated_at = GetStr(root, "generated_at");
    return true;
}

auto BuildPairBody(const std::string& code, const std::string& name, const std::string& version, const std::string& firmware) -> std::string {
    MutDoc doc{yyjson_mut_doc_new(nullptr)};
    const auto root = yyjson_mut_obj(doc.get());
    yyjson_mut_doc_set_root(doc.get(), root);

    yyjson_mut_obj_add_strcpy(doc.get(), root, "code", code.c_str());
    const auto client = yyjson_mut_obj_add_obj(doc.get(), root, "client");
    yyjson_mut_obj_add_str(doc.get(), client, "platform", "switch");
    yyjson_mut_obj_add_strcpy(doc.get(), client, "name", name.c_str());
    yyjson_mut_obj_add_strcpy(doc.get(), client, "version", version.c_str());
    if (!firmware.empty()) {
        yyjson_mut_obj_add_strcpy(doc.get(), client, "firmware", firmware.c_str());
    }

    return Write(doc);
}

auto BuildConsoleState(const ConsoleState& state) -> std::string {
    MutDoc doc{yyjson_mut_doc_new(nullptr)};
    const auto d = doc.get();
    const auto root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);

    yyjson_mut_obj_add_strcpy(d, root, "platform", state.platform.c_str());
    if (state.firmware.empty()) {
        yyjson_mut_obj_add_null(d, root, "firmware");
    } else {
        yyjson_mut_obj_add_strcpy(d, root, "firmware", state.firmware.c_str());
    }

    const auto storages = yyjson_mut_obj_add_arr(d, root, "storages");
    for (const auto& s : state.storages) {
        const auto o = yyjson_mut_arr_add_obj(d, storages);
        yyjson_mut_obj_add_strcpy(d, o, "id", s.id.c_str());
        yyjson_mut_obj_add_strcpy(d, o, "label", s.label.c_str());
        yyjson_mut_obj_add_int(d, o, "free", s.free);
        yyjson_mut_obj_add_int(d, o, "total", s.total);
    }

    const auto installed = yyjson_mut_obj_add_arr(d, root, "installed");
    for (const auto& e : state.installed) {
        const auto o = yyjson_mut_arr_add_obj(d, installed);
        yyjson_mut_obj_add_strcpy(d, o, "title_id", e.title_id.c_str());
        yyjson_mut_obj_add_strcpy(d, o, "version", e.version.c_str());
        yyjson_mut_obj_add_str(d, o, "content", ContentName(e.content));
        yyjson_mut_obj_add_strcpy(d, o, "storage", e.storage.c_str());
    }

    return Write(doc);
}

auto BuildReport(const Report& report) -> std::string {
    MutDoc doc{yyjson_mut_doc_new(nullptr)};
    const auto d = doc.get();
    const auto root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);

    yyjson_mut_obj_add_str(d, root, "result", ReportResultName(report.result));
    if (report.storage.empty()) {
        yyjson_mut_obj_add_null(d, root, "storage");
    } else {
        yyjson_mut_obj_add_strcpy(d, root, "storage", report.storage.c_str());
    }
    if (report.detail.empty()) {
        yyjson_mut_obj_add_null(d, root, "detail");
    } else {
        yyjson_mut_obj_add_strcpy(d, root, "detail", report.detail.c_str());
    }
    yyjson_mut_obj_add_int(d, root, "duration_s", report.duration_s);

    return Write(doc);
}

auto IsSafePath(std::string_view path) -> bool {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
        return false;
    }

    size_t start = 0;
    while (start <= path.size()) {
        const auto end = std::min(path.find('/', start), path.size());
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

auto FormatTitleId(std::uint64_t id) -> std::string {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(id));
    return buf;
}

auto ParseTitleId(std::string_view text, std::uint64_t& out) -> bool {
    if (text.size() != 16) {
        return false;
    }

    std::uint64_t v = 0;
    for (const auto c : text) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
    }

    out = v;
    return true;
}

auto ContentOfTitleId(std::uint64_t id) -> Content {
    const auto low = id & 0xFFF;
    const bool bit12 = id & 0x1000;
    if (low == 0x000 && !bit12) {
        return Content::Base;
    }
    if (low == 0x800) {
        return Content::Update;
    }
    if (bit12 && low != 0) {
        return Content::Dlc;
    }
    return Content::Unknown;
}

auto FormatFirmware(int major, int minor) -> std::string {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d.%02d", major, minor);
    return buf;
}

auto Fits(std::int64_t size, std::int64_t free) -> bool {
    if (size < 0 || free < 0) {
        return false;
    }
    // size + 5%, rounded up, without overflowing on anything a disk can hold.
    const auto needed = size + (size / 100) * SPACE_MARGIN_PERCENT + ((size % 100) * SPACE_MARGIN_PERCENT + 99) / 100;
    return needed <= free;
}

auto GroupRows(const std::vector<CatalogItem>& items) -> std::vector<Row> {
    // the games, in the order their first row appears in the catalog - which the
    // mac sorts by title. a game whose base isn't in the catalog is placed by its
    // first update or dlc, under that one's title.
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::size_t>> members;
    std::map<std::string, std::string> titles;

    for (std::size_t i = 0; i < items.size(); i++) {
        const auto key = GroupKey(items[i]);
        auto& m = members[key];
        if (m.empty()) {
            order.emplace_back(key);
        }
        m.emplace_back(i);

        if (items[i].content == Content::Base || !titles.contains(key)) {
            titles[key] = items[i].title;
        }
    }

    // re-sorted by the title the game will be drawn under, which for a game with
    // a base is the base's own: an update named "Jogo [v2]" sorts by "Jogo".
    std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
        const auto& ta = titles[a];
        const auto& tb = titles[b];
        return std::lexicographical_compare(ta.begin(), ta.end(), tb.begin(), tb.end(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    });

    std::vector<Row> rows;
    rows.reserve(items.size());

    for (const auto& key : order) {
        auto m = members[key];
        // base → update → dlc; newest update first among updates, so the one a
        // person most likely wants sits right under the game.
        std::stable_sort(m.begin(), m.end(), [&](std::size_t a, std::size_t b) {
            const auto ra = ContentRank(items[a].content);
            const auto rb = ContentRank(items[b].content);
            if (ra != rb) {
                return ra < rb;
            }
            if (items[a].content == Content::Update) {
                return VersionNumber(items[a].version) > VersionNumber(items[b].version);
            }
            return false;
        });

        const bool has_base = std::any_of(m.begin(), m.end(), [&](std::size_t i) {
            return items[i].content == Content::Base;
        });

        for (const auto i : m) {
            Row row{};
            row.item = i;
            row.group = key;
            row.depth = (has_base && items[i].content != Content::Base) ? 1 : 0;
            rows.emplace_back(std::move(row));
        }
    }

    return rows;
}

auto InstallOrder(const std::vector<CatalogItem>& items, const std::vector<std::size_t>& chosen) -> std::vector<std::size_t> {
    std::vector<std::size_t> out;
    out.reserve(chosen.size());

    // walking the grouped rows keeps a game's base ahead of what needs it. among
    // several updates for one game, the oldest goes first: each one installs over
    // the last, so the newest one chosen is what the console ends up with.
    const auto rows = GroupRows(items);
    std::vector<std::size_t> updates;
    std::string group;

    const auto flush_updates = [&]() {
        std::sort(updates.begin(), updates.end(), [&](std::size_t a, std::size_t b) {
            return VersionNumber(items[a].version) < VersionNumber(items[b].version);
        });
        out.insert(out.end(), updates.begin(), updates.end());
        updates.clear();
    };

    for (const auto& row : rows) {
        if (row.group != group) {
            flush_updates();
            group = row.group;
        }

        if (std::find(chosen.begin(), chosen.end(), row.item) == chosen.end()) {
            continue;
        }

        if (items[row.item].content == Content::Update) {
            updates.emplace_back(row.item);
        } else {
            // dlc comes after every update of the same game, since rows already
            // run base → update → dlc.
            if (items[row.item].content != Content::Base) {
                flush_updates();
            }
            out.emplace_back(row.item);
        }
    }
    flush_updates();

    return out;
}

auto Companions(const std::vector<CatalogItem>& items, std::size_t base) -> std::vector<std::size_t> {
    std::vector<std::size_t> out{base};
    if (base >= items.size() || items[base].content != Content::Base || items[base].title_id.empty()) {
        return out;
    }

    const auto& game = items[base].title_id;
    const auto wanted = [](const CatalogItem& e) {
        return e.state != State::Installed && e.state != State::Incompatible;
    };

    // only the newest update: each one carries everything before it.
    std::size_t newest = items.size();
    for (std::size_t i = 0; i < items.size(); i++) {
        const auto& e = items[i];
        if (e.parent_title_id != game || !wanted(e)) {
            continue;
        }

        if (e.content == Content::Update) {
            if (newest == items.size() || VersionNumber(e.version) > VersionNumber(items[newest].version)) {
                newest = i;
            }
        } else if (e.content == Content::Dlc) {
            out.emplace_back(i);
        }
    }

    if (newest != items.size()) {
        out.emplace_back(newest);
    }

    return out;
}

auto InstallPathFor(const CatalogItem& item) -> std::string {
    return "ponte." + (item.kind.empty() ? std::string{"nsp"} : item.kind);
}

auto RouteInfo() -> std::string {
    return std::string{API_PREFIX} + "/info";
}

auto RoutePair() -> std::string {
    return std::string{API_PREFIX} + "/pair";
}

auto RouteCatalog(std::string_view platform) -> std::string {
    return std::string{API_PREFIX} + "/catalog?platform=" + std::string{platform};
}

auto RouteConsoleState() -> std::string {
    return std::string{API_PREFIX} + "/console/state";
}

auto RouteItem(std::string_view id) -> std::string {
    return std::string{API_PREFIX} + "/items/" + std::string{id};
}

auto RouteFile(std::string_view id, std::int64_t index) -> std::string {
    return RouteItem(id) + "/files/" + std::to_string(index);
}

auto RouteReport(std::string_view id) -> std::string {
    return RouteItem(id) + "/report";
}

auto RouteChanges(std::string_view since) -> std::string {
    return std::string{API_PREFIX} + "/catalog/changes?since=" + std::string{since};
}

auto BaseUrl(std::string_view address) -> std::string {
    // trimmed of anything a person might type around the address itself.
    while (!address.empty() && std::isspace(static_cast<unsigned char>(address.front()))) address.remove_prefix(1);
    while (!address.empty() && (std::isspace(static_cast<unsigned char>(address.back())) || address.back() == '/')) address.remove_suffix(1);
    if (address.starts_with("http://")) {
        address.remove_prefix(7);
    }
    if (address.empty() || address.find('/') != std::string_view::npos || address.starts_with("https://")) {
        return {};
    }

    std::string out{"http://"};
    out += address;
    // a bare host gets the default port; "host:port" keeps its own.
    if (address.find(':') == std::string_view::npos) {
        out += ":" + std::to_string(DEFAULT_PORT);
    }
    return out;
}

} // namespace sphaira::ponte
