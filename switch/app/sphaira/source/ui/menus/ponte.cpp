#include "ui/menus/ponte.hpp"

#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/popup_list.hpp"
#include "ui/progress_box.hpp"
#include "yati/yati.hpp"
#include "yati/source/http.hpp"
#include "utils/utils.hpp"

#include "app.hpp"
#include "defines.hpp"
#include "download.hpp"
#include "evman.hpp"
#include "fs.hpp"
#include "i18n.hpp"
#include "image.hpp"
#include "log.hpp"
#include "swkbd.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdio>
#include <map>
#include <utility>

namespace sphaira::ui::menu::ponte {
namespace {

namespace pn = sphaira::ponte;

constexpr auto CACHE_PATH = "/switch/sphaira/cache/ponte";
// how often the library asks the mac whether anything was added.
constexpr u64 POLL_SECONDS = 20;
// icon decodes per frame, so scrolling a fresh page doesn't stall.
constexpr int ICON_BUDGET = 2;

constexpr float ROW_H = 68.f;
constexpr float CHILD_INDENT = 44.f;

std::atomic_uint32_t g_installs{};

auto ContentLabel(pn::Content content) -> std::string {
    switch (content) {
        case pn::Content::Base: return "Game"_i18n;
        case pn::Content::Update: return "Update"_i18n;
        case pn::Content::Dlc: return "DLC"_i18n;
        case pn::Content::Unknown: break;
    }
    return "Unknown"_i18n;
}

auto StateLabel(const pn::CatalogItem& item) -> std::string {
    switch (item.state) {
        case pn::State::New: return "New"_i18n;
        case pn::State::Installed: return "Installed"_i18n;
        case pn::State::UpdateAvailable: return "Newer than installed"_i18n;
        case pn::State::Incompatible: return "Incompatible"_i18n;
        case pn::State::None: break;
    }
    return {};
}

auto StateColour(const pn::CatalogItem& item, Theme* theme) -> NVGcolor {
    switch (item.state) {
        case pn::State::New: return theme->GetColour(ThemeEntryID_HIGHLIGHT_1);
        case pn::State::UpdateAvailable: return theme->GetColour(ThemeEntryID_HIGHLIGHT_2);
        case pn::State::Incompatible: return theme->GetColour(ThemeEntryID_ERROR);
        default: return theme->GetColour(ThemeEntryID_TEXT_INFO);
    }
}

auto WarningLabel(const std::string& code) -> std::string {
    if (code == "metadata_from_filename") return "name from file"_i18n;
    if (code == "duplicate_title_id") return "duplicate"_i18n;
    if (code == "experimental_format") return "experimental"_i18n;
    if (code == "unknown_compatibility") return "untested"_i18n;
    return {};
}

// one line under the title: what the item is, its version, format and size.
auto DescribeItem(const pn::CatalogItem& item) -> std::string {
    std::string out = ContentLabel(item.content);
    if (item.content != pn::Content::Base && !item.version.empty()) {
        out += " v" + item.version;
    }
    std::string kind = item.kind;
    std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    out += "  •  " + kind + "  •  " + utils::formatSizeStorage(item.size);
    for (const auto& w : item.warnings) {
        if (const auto label = WarningLabel(w); !label.empty()) {
            out += "  •  " + label;
        }
    }
    return out;
}

auto IconCachePath(const pn::CatalogItem& item) -> fs::FsPath {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%s_%s.img", CACHE_PATH, item.id.c_str(), item.revision.c_str());
    return path;
}

// what an install of these needs, on top of what is free.
auto TotalSize(const std::vector<pn::CatalogItem>& items, const std::vector<std::size_t>& chosen) -> s64 {
    s64 total{};
    for (const auto i : chosen) {
        total += items[i].size;
    }
    return total;
}

} // namespace

Menu::Icon::Icon(Icon&& rhs) noexcept {
    *this = std::move(rhs);
}

auto Menu::Icon::operator=(Icon&& rhs) noexcept -> Icon& {
    std::swap(image, rhs.image);
    std::swap(state, rhs.state);
    std::swap(tried_cache, rhs.tried_cache);
    return *this;
}

Menu::Icon::~Icon() {
    if (image) {
        nvgDeleteImage(App::GetVg(), image);
    }
}

#ifdef PONTE_STANDALONE
#define PONTE_TITLE "Ponte"
#else
#define PONTE_TITLE "Mac library"_i18n
#endif

Menu::Menu(u32 flags) : MenuBase{PONTE_TITLE, flags} {
    const Vec4 mac_v{75, GetY() + 1.f + 42.f, 1220.f - 45.f * 2, 60};
    m_mac_list = std::make_unique<List>(1, 8, m_pos, mac_v);

    const Vec4 row_v{75, GetY() + 1.f + 42.f, 1220.f - 45.f * 2, ROW_H};
    m_row_list = std::make_unique<List>(1, 7, m_pos, row_v);

    LoadSaved();
    BuildMacRows();
    SetMode(Mode::Macs);
    // bound on the first Update, once MainMenu has handed a tab its own Y and '-'.
    m_actions_dirty = true;

}

void Menu::OfferHbmenuRestore() {
    App::Push<OptionBox>(
        "Ponte is running in place of the homebrew menu (/hbmenu.nro).\n"
        "An earlier version copied itself there by mistake.\n\n"
        "Restore the homebrew menu?"_i18n,
        "Not now"_i18n, "Restore"_i18n, 1, [](auto op_index) {
            if (!op_index || *op_index != 1) {
                return;
            }

            fs::FsNativeSd sd{};
            // the backup sphaira makes of the real hbmenu, then sphaira itself.
            const char* sources[] = { "/switch/hbmenu.nro", "/switch/sphaira/sphaira.nro", "/switch/sphaira.nro" };
            for (const auto src : sources) {
                if (!sd.FileExists(src)) {
                    continue;
                }

                if (R_SUCCEEDED(sd.copy_entire_file("/hbmenu.nro", src))) {
                    // romfs came from the file just replaced: nothing more can be
                    // loaded from it, so this ends here.
                    App::Push<OptionBox>(
                        i18n::Reorder("Homebrew menu restored from ", src) + "\n\n" +
                        "Ponte will close now. Open it again from the homebrew menu."_i18n,
                        "OK"_i18n, [](auto) {
                            App::Exit();
                        }
                    );
                    return;
                }
            }

            App::Push<OptionBox>(
                "No copy of the homebrew menu was found on the card.\n\n"
                "On a computer, copy hbmenu.nro (from switchbrew/nx-hbmenu) "
                "or sphaira.nro to the root of the card as /hbmenu.nro."_i18n,
                "OK"_i18n
            );
        }
    );
}

Menu::~Menu() {
    // nothing queued for the main thread may run once this is gone, and the
    // workers are joined before the members they write to go away.
    m_life.request_stop();
    m_work_stop.request_stop();
    m_icon_stop.request_stop();
    m_work.reset();
    m_poll.reset();
    m_discovery.reset();
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    // rebound here, ahead of the buttons being read, and never from inside an
    // action's own callback.
    if (m_actions_dirty) {
        ApplyActions();
    }

#ifdef PONTE_STANDALONE
    // asked on the first frame, once this menu is on the stack: a box pushed
    // from the constructor would end up underneath it.
    if (!m_hbmenu_checked) {
        m_hbmenu_checked = true;
        if (App::IsHbmenu()) {
            OfferHbmenuRestore();
            return;
        }
    }
#endif

    MenuBase::Update(controller, touch);

    if (m_busy) {
        return;
    }

    if (m_mode == Mode::Macs) {
        m_mac_list->OnUpdate(controller, touch, m_mac_index, m_macs.size(), [this](bool touch, auto i) {
            if (touch && m_mac_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_mac_index = i;
                UpdateActions();
            }
        });
    } else {
        m_row_list->OnUpdate(controller, touch, m_row_index, m_rows.size(), [this](bool touch, auto i) {
            if (touch && m_row_index == i) {
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_row_index = i;
                UpdateActions();
            }
        });

        PollChanges();
    }
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    if (m_busy) {
        // girando enquanto espera o Mac: dá para ver que não travou.
        gfx::drawSpinner(vg, theme, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f - 60.f, 26.f, armTicksToNs(armGetSystemTick()) / 1e+9);
        DrawCentered(vg, theme, m_busy_text);
        return;
    }

    if (m_mode == Mode::Macs) {
        DrawMacs(vg, theme);
    } else {
        DrawLibrary(vg, theme);
    }

    // aberto pelo Álbum, o sistema dá ao app bem menos memória de rede: a cópia fica lenta.
    if (App::IsApplet()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, GetY() + 21.f, 18.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_ERROR), "%s",
            "Opened from the Album: the network is much slower. Open it holding R on a game."_i18n.c_str());
    }
}

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    // MainMenu hands its own Y and '-' back to a tab right after this.
    UpdateActions();

    // the first look goes to the network, since a mac never announces itself.
    if (m_mode == Mode::Macs && !m_searched) {
        StartDiscovery();
    }

    // something was installed from elsewhere (or by this menu, whose progress
    // box just closed): the states on screen are stale.
    if (m_mode == Mode::Library && !m_working && m_installs_seen != g_installs) {
        Refresh(true);
    }
}

void Menu::SetMode(Mode mode) {
    m_mode = mode;
    if (mode == Mode::Macs) {
        // a quiet refresh still running belongs to the library being left.
        m_work_stop.request_stop();
        m_icon_stop.request_stop();
        FreeIcons();
        m_catalog = {};
        m_rows.clear();
        m_chosen.clear();
        m_row_index = 0;
        m_row_list->SetYoff(0);
        SetTitle(PONTE_TITLE);
    } else {
        SetTitle(m_server.name.empty() ? std::string{PONTE_TITLE} : m_server.name);
        m_last_poll.Update();
    }
    UpdateActions();
}

void Menu::UpdateActions() {
    m_actions_dirty = true;
    UpdateSubheading();
}

void Menu::Borrow(Button button, std::optional<Action>& prev, bool& borrowed, const Action& action) {
    if (!borrowed) {
        const auto it = m_actions.find(button);
        prev = it == m_actions.end() ? std::nullopt : std::optional<Action>{it->second};
        borrowed = true;
    }
    SetAction(button, action);
}

void Menu::GiveBack(Button button, std::optional<Action>& prev, bool& borrowed) {
    if (!borrowed) {
        return;
    }
    if (prev) {
        SetAction(button, *prev);
    } else {
        RemoveAction(button);
    }
    prev.reset();
    borrowed = false;
}

void Menu::ApplyActions() {
    m_actions_dirty = false;

    // only the buttons this menu owns; L, R and START stay MainMenu's.
    RemoveAction(Button::A);
    RemoveAction(Button::B);
    RemoveAction(Button::X);

    std::optional<Action> y{};
    std::optional<Action> select{};

    const bool can_leave = !IsTab() || m_mode == Mode::Library || m_busy;
    if (can_leave) {
#ifdef PONTE_STANDALONE
        const auto back = m_busy ? "Cancel"_i18n : (m_mode == Mode::Macs ? "Exit"_i18n : "Back"_i18n);
#else
        const auto back = m_busy ? "Cancel"_i18n : "Back"_i18n;
#endif
        SetAction(Button::B, Action{back, [this](){
            if (m_busy) {
                m_work_stop.request_stop();
                m_busy = false;
                UpdateActions();
                return;
            }

            if (m_mode == Mode::Library) {
                SetMode(Mode::Macs);
            } else {
                SetPop();
            }
        }});
    }

    if (!m_busy && m_mode == Mode::Macs) {
        if (!m_macs.empty()) {
            SetAction(Button::A, Action{"Connect"_i18n, [this](){
                if (m_mac_index >= 0 && m_mac_index < static_cast<s64>(m_macs.size())) {
                    Connect(m_macs[m_mac_index].server);
                }
            }});
        }

        SetAction(Button::X, Action{"Search"_i18n, [this](){
            StartDiscovery();
        }});

        y = Action{"Add by address"_i18n, [this](){
            PromptAddress();
        }};

        if (m_mac_index < static_cast<s64>(m_macs.size()) && m_macs[m_mac_index].saved) {
            select = Action{"Forget"_i18n, [this](){
                ForgetSelected();
            }};
        }
    } else if (!m_busy) {
        if (!m_rows.empty()) {
            SetAction(Button::A, Action{m_chosen[m_rows[m_row_index].item] ? "Unselect"_i18n : "Select"_i18n, [this](){
                ToggleRow();
            }});

            const auto count = ChosenItems().size();
            SetAction(Button::X, Action{count ? i18n::Reorder("Install ", std::to_string(count)) : "Install"_i18n, [this](){
                InstallChosen();
            }});

            const bool any = std::find(m_chosen.begin(), m_chosen.end(), 1) != m_chosen.end();
            select = Action{any ? "Clear"_i18n : "Select new"_i18n, [this, any](){
                if (any) {
                    ClearSelection();
                } else {
                    SelectAllNew();
                }
            }};
        }

        y = Action{"Refresh"_i18n, [this](){
            Refresh();
        }};
    }

#ifdef PONTE_STANDALONE
    // o app Ponte é só esta tela: + sai e ZR abre as opções do motor
    // (idioma, tema, instalação no microSD ou na memória, rede).
    SetAction(Button::START, Action{"Exit"_i18n, [](){
        App::Exit();
    }});
    SetAction(Button::R2, Action{"Options"_i18n, [](){
        App::DisplayMenuOptions();
    }});
#endif

    if (y) {
        Borrow(Button::Y, m_prev_y, m_borrowed_y, *y);
    } else {
        GiveBack(Button::Y, m_prev_y, m_borrowed_y);
    }

    if (select) {
        Borrow(Button::SELECT, m_prev_select, m_borrowed_select, *select);
    } else {
        GiveBack(Button::SELECT, m_prev_select, m_borrowed_select);
    }
}

void Menu::UpdateSubheading() {
    // A linha de baixo fica só para os comandos (são até 7 e, em português, ocupam
    // quase a largura toda); as contagens vão para o cabeçalho, ao lado do título.
    // O espaço livre no microSD já aparece no canto de cima, na barra do motor.
    SetSubHeading("");

    if (m_mode == Mode::Macs) {
        SetTitleSubHeading("Ponte: games from your Mac"_i18n + "  •  " + std::to_string(m_macs.size()) + " " + "Mac(s)"_i18n);
        return;
    }

    const auto chosen = ChosenItems();
    std::string sub = m_server.address + "  •  " + std::to_string(m_catalog.items.size()) + " " + "items"_i18n;
    if (!chosen.empty()) {
        sub += "  •  " + std::to_string(chosen.size()) + " " + "selected"_i18n + " (" + utils::formatSizeStorage(TotalSize(m_catalog.items, chosen)) + ")";
    }
    SetTitleSubHeading(sub);
}

void Menu::LoadSaved() {
    pn::LoadServers(m_saved);
}

void Menu::BuildMacRows() {
    m_macs.clear();

    for (const auto& saved : m_saved) {
        MacRow row{};
        row.server = saved;
        row.saved = true;
        // the network's answer beats the address that was saved: a mac on dhcp
        // moves, and discovery is the only one that knows where it went.
        const auto it = std::find_if(m_found.begin(), m_found.end(), [&](const auto& f) { return f.id == saved.id; });
        if (it != m_found.end()) {
            row.found = true;
            row.server.address = it->address;
            if (!it->name.empty()) {
                row.server.name = it->name;
            }
        }
        m_macs.emplace_back(std::move(row));
    }

    for (const auto& found : m_found) {
        const auto saved = std::any_of(m_saved.begin(), m_saved.end(), [&](const auto& s) { return s.id == found.id; });
        if (saved) {
            continue;
        }

        MacRow row{};
        row.server.id = found.id;
        row.server.name = found.name;
        row.server.address = found.address;
        row.found = true;
        m_macs.emplace_back(std::move(row));
    }

    m_mac_index = std::clamp<s64>(m_mac_index, 0, std::max<s64>(0, static_cast<s64>(m_macs.size()) - 1));
}

void Menu::StartDiscovery() {
    if (m_discovering) {
        return;
    }

    m_discovering = true;
    m_searched = true;
    const auto life = m_life.get_token();

    m_discovery = std::make_unique<utils::Async>([this, life](){
        auto found = pn::Discover(life);
        evman::push(evman::CallbackEventData{[this, found = std::move(found)]() mutable {
            m_found = std::move(found);
            m_discovering = false;
            BuildMacRows();
            UpdateActions();
        }, life}, false);
    });
}

void Menu::PromptAddress() {
    std::string address;
    if (R_FAILED(swkbd::ShowText(address, "Mac address"_i18n.c_str(), "192.168.0.10 or 192.168.0.10:47101"_i18n.c_str())) || address.empty()) {
        return;
    }

    if (pn::BaseUrl(address).empty()) {
        ShowMessage("That doesn't look like an address. Use the IP shown in the Ponte app on the Mac."_i18n);
        return;
    }

    pn::Server server{};
    server.address = address;
    server.name = address;
    Connect(server);
}

void Menu::ForgetSelected() {
    if (m_mac_index < 0 || m_mac_index >= static_cast<s64>(m_macs.size()) || !m_macs[m_mac_index].saved) {
        return;
    }

    const auto server = m_macs[m_mac_index].server;
    App::Push<OptionBox>(
        i18n::Reorder("Forget ", server.name) + "?\n\n" + "You will need the pairing code again to connect."_i18n,
        "Back"_i18n, "Forget"_i18n, 0, [this, id = server.id](auto op_index) {
            if (op_index && *op_index == 1) {
                pn::DeleteServer(id);
                LoadSaved();
                BuildMacRows();
                UpdateActions();
            }
        }
    );
}

void Menu::PromptCode(const pn::Server& server) {
    s64 code{};
    const auto header = i18n::Reorder("Pair with ", server.name);
    if (R_FAILED(swkbd::ShowNumPad(code, header.c_str(), "Enter the 6 digit code shown on the Mac"_i18n.c_str(), nullptr, 6, 6))) {
        return;
    }

    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06ld", code);
    Connect(server, buf);
}

void Menu::Connect(const pn::Server& server, const std::string& code) {
    // a worker still unwinding from a cancel answers shortly; replacing it now
    // would block the main thread on its join.
    if (m_working) {
        return;
    }

    m_working = true;
    m_busy = true;
    m_busy_text = i18n::Reorder("Connecting to ", server.name.empty() ? server.address : server.name) + "...";
    m_work_stop = {};
    UpdateActions();

    const auto token = m_work_stop.get_token();
    const auto life = m_life.get_token();

    const auto saved = m_saved;
    m_work = std::make_unique<utils::Async>([this, server, code, token, life, saved](){
        auto connected = server;
        pn::Failure fail{};
        pn::Catalog catalog{};
        pn::ConsoleState state{};
        auto outcome = ConnectOutcome::Failed;

        [&]() {
            pn::ServerInfo info{};
            if (!pn::Client{connected}.Info(info, fail, token)) {
                return;
            }

            // a saved row's address now answered by another mac: its token
            // belongs to the old one and is not sent here.
            if (!connected.id.empty() && connected.id != info.id) {
                connected.token.clear();
            }

            // the mac's own id and name win over anything saved or typed.
            connected.id = info.id;

            // a typed address for a mac already paired keeps its pairing.
            if (connected.token.empty()) {
                const auto it = std::find_if(saved.begin(), saved.end(), [&](const auto& e) { return e.id == info.id; });
                if (it != saved.end()) {
                    connected.token = it->token;
                }
            }
            if (!info.name.empty()) {
                connected.name = info.name;
            }

            // a mac with pairing off answers with an empty token, which is
            // also what is sent from then on.
            if (info.pairing_required && connected.token.empty()) {
                if (code.empty()) {
                    outcome = ConnectOutcome::NeedCode;
                    return;
                }
                if (!pn::Client{connected}.Pair(code, connected, fail, token)) {
                    return;
                }
                // kept at once: the mac has already moved on to a new code.
                pn::SaveServer(connected);
            }

            state = pn::ScanConsole();
            if (!pn::Client{connected}.PostState(state, catalog, fail, token)) {
                // the mac forgot this console (it was revoked there): pair again.
                if (fail.status == 401 && !connected.token.empty()) {
                    connected.token.clear();
                    pn::SaveServer(connected);
                    outcome = ConnectOutcome::NeedCode;
                }
                return;
            }

            // saved here, on the worker: minIni rewrites the file once per key.
            pn::SaveServer(connected);
            outcome = ConnectOutcome::Connected;
        }();

        evman::push(evman::CallbackEventData{[this, token, outcome, connected, fail, catalog = std::move(catalog), state = std::move(state)]() mutable {
            m_working = false;
            // B cancelled this one; whatever it found is dropped.
            if (token.stop_requested()) {
                return;
            }

            m_busy = false;
            LoadSaved();
            BuildMacRows();

            switch (outcome) {
                case ConnectOutcome::Connected:
                    m_server = connected;
                    m_installs_seen = g_installs;
                    SetMode(Mode::Library);
                    ApplyCatalog(std::move(catalog), std::move(state));
                    break;

                case ConnectOutcome::NeedCode:
                    UpdateActions();
                    PromptCode(connected);
                    break;

                case ConnectOutcome::Failed:
                    UpdateActions();
                    ShowMessage(fail.message);
                    break;
            }
        }, life}, false);
    });
}

void Menu::Refresh(bool quiet) {
    if (m_working || m_mode != Mode::Library) {
        return;
    }

    m_working = true;
    m_busy = !quiet;
    m_busy_text = "Asking the Mac..."_i18n;
    m_work_stop = {};
    UpdateActions();

    const auto token = m_work_stop.get_token();
    const auto life = m_life.get_token();
    const auto server = m_server;
    const auto installs = g_installs.load();

    m_work = std::make_unique<utils::Async>([this, server, token, life, quiet, installs](){
        auto state = pn::ScanConsole();
        pn::Catalog catalog{};
        pn::Failure fail{};
        const auto ok = pn::Client{server}.PostState(state, catalog, fail, token);

        evman::push(evman::CallbackEventData{[this, token, ok, fail, quiet, installs, catalog = std::move(catalog), state = std::move(state)]() mutable {
            m_working = false;
            if (token.stop_requested() || m_mode != Mode::Library) {
                return;
            }

            m_busy = false;
            m_last_poll.Update();

            if (!ok) {
                UpdateActions();
                // a quiet refresh that fails leaves the list as it was; the
                // next look will try again.
                if (!quiet) {
                    ShowMessage(fail.message);
                }
                return;
            }

            m_installs_seen = installs;
            ApplyCatalog(std::move(catalog), std::move(state));
        }, life}, false);
    });
}

void Menu::ApplyCatalog(pn::Catalog&& catalog, pn::ConsoleState&& state) {
    // what was ticked stays ticked across a refresh, unless it is now installed.
    std::map<std::string, bool> kept;
    for (std::size_t i = 0; i < m_catalog.items.size() && i < m_chosen.size(); i++) {
        if (m_chosen[i]) {
            kept[m_catalog.items[i].id] = true;
        }
    }

    std::string current;
    if (m_row_index >= 0 && m_row_index < static_cast<s64>(m_rows.size())) {
        current = m_catalog.items[m_rows[m_row_index].item].id;
    }

    m_icon_stop.request_stop();
    m_icon_stop = {};
    FreeIcons();

    m_catalog = std::move(catalog);
    m_console = std::move(state);
    m_rows = pn::GroupRows(m_catalog.items);
    m_chosen.assign(m_catalog.items.size(), 0);
    m_icons.clear();
    m_icons.resize(m_catalog.items.size());

    for (std::size_t i = 0; i < m_catalog.items.size(); i++) {
        const auto& e = m_catalog.items[i];
        if (kept.contains(e.id) && e.state != pn::State::Installed && e.state != pn::State::Incompatible) {
            m_chosen[i] = 1;
        }
    }

    m_row_index = 0;
    for (std::size_t r = 0; r < m_rows.size(); r++) {
        if (m_catalog.items[m_rows[r].item].id == current) {
            m_row_index = r;
            break;
        }
    }
    if (!m_row_index) {
        m_row_list->SetYoff(0);
    }

    fs::FsNativeSd().CreateDirectoryRecursively(CACHE_PATH);
    UpdateActions();
}

void Menu::PollChanges() {
    if (m_polling || m_working || m_catalog.generated_at.empty() || m_last_poll.GetSeconds() < POLL_SECONDS) {
        return;
    }

    m_polling = true;
    m_last_poll.Update();

    const auto life = m_life.get_token();
    const auto server = m_server;
    const auto since = m_catalog.generated_at;

    m_poll = std::make_unique<utils::Async>([this, server, since, life](){
        pn::Changes changes{};
        pn::Failure fail{};
        const auto ok = pn::Client{server}.GetChanges(since, changes, fail, life);

        evman::push(evman::CallbackEventData{[this, ok, changes, id = server.id]() {
            m_polling = false;
            // the catalog moved on, or this is another mac by now.
            if (!ok || !changes.changed || m_mode != Mode::Library || m_server.id != id) {
                return;
            }

            if (changes.new_items > 0) {
                App::Notify(i18n::Reorder("New on the Mac: ", std::to_string(changes.new_items)));
            }
            Refresh(true);
        }, life}, false);
    });
}

auto Menu::ChosenItems() const -> std::vector<std::size_t> {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < m_chosen.size(); i++) {
        if (m_chosen[i]) {
            out.emplace_back(i);
        }
    }
    return out;
}

auto Menu::FreeOn(const char* storage) const -> s64 {
    for (const auto& s : m_console.storages) {
        if (s.id == storage) {
            return s.free;
        }
    }
    return 0;
}

void Menu::ToggleRow() {
    if (m_rows.empty()) {
        return;
    }

    const auto index = m_rows[m_row_index].item;
    const auto& item = m_catalog.items[index];

    if (m_chosen[index]) {
        m_chosen[index] = 0;
        UpdateActions();
        return;
    }

    if (item.state == pn::State::Incompatible) {
        ShowMessage(item.state_reason.empty() ? "This item can't be installed on this console."_i18n : item.state_reason);
        return;
    }

    // a game picked that isn't on the console brings along what makes sense
    // with it: its newest update and the dlc the console hasn't got.
    if (item.content == pn::Content::Base && item.state != pn::State::Installed) {
        for (const auto i : pn::Companions(m_catalog.items, index)) {
            m_chosen[i] = 1;
        }
    } else {
        m_chosen[index] = 1;
    }

    UpdateActions();
}

void Menu::SelectAllNew() {
    for (std::size_t i = 0; i < m_catalog.items.size(); i++) {
        const auto state = m_catalog.items[i].state;
        if (state == pn::State::New || state == pn::State::UpdateAvailable) {
            m_chosen[i] = 1;
        }
    }
    UpdateActions();
}

void Menu::ClearSelection() {
    std::fill(m_chosen.begin(), m_chosen.end(), 0);
    UpdateActions();
}

void Menu::InstallChosen() {
    if (m_rows.empty()) {
        return;
    }

    if (!App::GetInstallEnable()) {
        App::ShowEnableInstallPrompt();
        return;
    }

    auto chosen = ChosenItems();
    // nothing ticked installs the row under the cursor.
    if (chosen.empty()) {
        const auto index = m_rows[m_row_index].item;
        if (m_catalog.items[index].state == pn::State::Incompatible) {
            ToggleRow(); // shows why.
            return;
        }
        chosen.emplace_back(index);
    }

    const auto order = pn::InstallOrder(m_catalog.items, chosen);
    const auto size = TotalSize(m_catalog.items, order);
    std::vector<pn::CatalogItem> chosen_items;
    for (const auto i : order) {
        chosen_items.emplace_back(m_catalog.items[i]);
    }

    // where to: the sd card first, as sphaira's own setting has it.
    PopupList::Items items{
        "microSD card"_i18n + " (" + utils::formatSizeStorage(FreeOn("sd")) + " " + "free"_i18n + ")",
        "System memory"_i18n + " (" + utils::formatSizeStorage(FreeOn("nand")) + " " + "free"_i18n + ")",
    };

    App::Push<PopupList>("Where to install"_i18n + " (" + utils::formatSizeStorage(size) + ")", items, [this, chosen_items, size](auto op_index) {
        if (!op_index) {
            return;
        }

        const bool sd = *op_index == 0;
        const auto free = FreeOn(sd ? "sd" : "nand");
        if (!pn::Fits(size, free)) {
            ShowMessage(i18n::Reorder("Not enough space. Needed: ", utils::formatSizeStorage(size)) + "\n" +
                i18n::Reorder("Free: ", utils::formatSizeStorage(free)));
            return;
        }

        StartInstall(chosen_items, sd);
    }, App::GetInstallSdEnable() ? 0 : 1);
}

void Menu::StartInstall(std::vector<pn::CatalogItem> items, bool sd) {
    // the box runs on a thread of its own and can outlive this menu, so it is
    // handed copies of everything it needs.
    if (items.empty()) {
        return;
    }

    struct Summary {
        std::vector<std::string> failed{};
        std::vector<std::string> done{};
        s64 installed{};
        bool cancelled{};
    };
    auto summary = std::make_shared<Summary>();
    const pn::Client client{m_server};
    const std::string storage = sd ? "sd" : "nand";

    // enquanto instala, o menu HOME não congela o app: a cópia continua por trás.
    // devolvido no fim da caixa, junto com o descanso automático.
    App::SetRunInBackground(true);
    const auto hint = App::CanRunInBackground()
        ? "HOME: the copy keeps going. Opening another game closes Ponte."_i18n
        : "Opened from the Album: the copy pauses on the HOME menu."_i18n;

    App::Push<ProgressBox>(0, "Installing "_i18n, items.front().title, [client, items, sd, storage, summary](ProgressBox* pbox) -> Result {
        for (std::size_t n = 0; n < items.size(); n++) {
            const auto& item = items[n];
            const auto started = armGetSystemTick();
            const auto elapsed = [started]() -> s64 {
                return armTicksToNs(armGetSystemTick() - started) / 1'000'000'000;
            };

            std::string title = item.title;
            if (item.content != pn::Content::Base) {
                title += " (" + ContentLabel(item.content) + (item.version.empty() ? "" : " v" + item.version) + ")";
            }
            pbox->SetTitle(title);
            pbox->SetActionName(i18n::Reorder("Installing ", std::to_string(n + 1) + "/" + std::to_string(items.size())));

            pn::ItemDetail detail{};
            pn::Failure fail{};
            Result rc{};
            std::string detail_text;

            if (!client.Item(item.id, detail, fail, pbox->GetToken())) {
                rc = Result_YatiHttpReadFailed;
                detail_text = fail.message;
            } else if (detail.files.size() != 1) {
                // every switch format is one file (SPEC §1.2).
                rc = Result_YatiContainerNotFound;
                detail_text = "unexpected file count";
            } else {
                // streamed straight into yati: no copy lands on the sd card first,
                // and a dropped connection resumes with Range (SPEC §8.1).
                yati::source::Http source{client.FileUrl(item.id, detail.files[0].index), client.FileHeaders(detail.item.revision), pbox->GetToken()};

                yati::Outcome outcome{};
                yati::ConfigOverride config_override{};
                config_override.sd_card_install = sd;
                // o estado vem do Mac, pelos registros do ns: um resto no ncm
                // (jogo apagado pela metade) não pode virar "já instalado".
                config_override.skip_if_already_installed = false;
                config_override.outcome = &outcome;
                rc = yati::InstallFromSource(pbox, &source, pn::InstallPathFor(detail.item), config_override);
                if (R_FAILED(rc)) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "yati 0x%X", rc);
                    detail_text = buf;
                } else if (!outcome.installed) {
                    // o yati termina "com sucesso" quando pula tudo: sem isto a
                    // tela dizia "concluída" depois de baixar só o cabeçalho.
                    rc = Result_YatiContainerNotFound;
                    detail_text = outcome.skipped
                        ? "Nothing was installed: "_i18n + i18n::get(outcome.skip_reason)
                        : "Nothing was installed: no content found in the file"_i18n;
                    log_write("[PONTE] nada instalado de %s (%u pulados): %s\n", item.id.c_str(), outcome.skipped, outcome.skip_reason.c_str());
                }
            }

            const bool cancelled = pbox->ShouldExit() || rc == Result_TransferCancelled;

            pn::Report report{};
            report.storage = storage;
            report.duration_s = elapsed();
            if (R_SUCCEEDED(rc)) {
                report.result = pn::ReportResult::Installed;
                summary->installed++;
                summary->done.emplace_back(title);
            } else if (cancelled) {
                report.result = pn::ReportResult::Cancelled;
            } else {
                report.result = pn::ReportResult::Failed;
                report.detail = detail_text;
                summary->failed.emplace_back(title + ": " + detail_text);
            }

            // sent on a token of its own: a cancelled box must still say so.
            std::stop_source report_stop{};
            client.SendReport(item.id, report, report_stop.get_token());

            if (cancelled) {
                summary->cancelled = true;
                return Result_TransferCancelled;
            }
        }

        // an item that failed doesn't stop the ones after it: an update or dlc
        // installs on its own, and the summary says what didn't.
        R_SUCCEED();
    }, [this, summary, total = items.size(), life = m_life.get_token()](Result) {
        App::SetRunInBackground(false);
        if (life.stop_requested()) {
            return;
        }

        // the end is always said in a box of its own: the progress box just
        // closes, and a toast in the corner is easy to miss from the couch.
        std::string text;
        if (summary->cancelled) {
            text = "Installation cancelled."_i18n;
        } else if (summary->failed.empty()) {
            App::PlaySoundEffect(SoundEffect::Install);
            text = "Installation complete!"_i18n;
        } else {
            text = "Installation finished with errors."_i18n;
        }
        text += "\n\n" + i18n::Reorder("Installed ", std::to_string(summary->installed) + "/" + std::to_string(total));
        for (const auto& d : summary->done) {
            text += "\n\u2022 " + d;
        }
        for (const auto& f : summary->failed) {
            text += "\n! " + f;
        }
        if (summary->installed) {
            text += "\n\n" + "It is already on the Switch HOME menu."_i18n;
        }
        ShowMessage(text);

        // installed items are no longer ticked, and the states change: ask the
        // mac again with what the console now holds.
        ClearSelection();
        Refresh(true);
    }, ProgressBoxOption::ScreenToggle, hint);
}

auto Menu::GetIcon(std::size_t index, int& budget) -> int {
    auto& icon = m_icons[index];
    const auto& item = m_catalog.items[index];

    if (icon.image) {
        return icon.image;
    }
    if (icon.state == IconState::Failed || item.icon.empty()) {
        return App::GetDefaultImage();
    }
    if (icon.state == IconState::Progress || budget <= 0) {
        return 0;
    }

    const auto path = IconCachePath(item);
    const auto load = [&]() -> bool {
        const auto data = ImageLoadFromFile(path);
        if (data.data.empty()) {
            return false;
        }
        icon.image = nvgCreateImageRGBA(App::GetVg(), data.w, data.h, 0, data.data.data());
        return icon.image != 0;
    };

    // keyed on the revision, so a cached icon is the item's current one.
    if (!icon.tried_cache) {
        icon.tried_cache = true;
        if (load()) {
            budget--;
            return icon.image;
        }
    }

    if (icon.state == IconState::Done) {
        budget--;
        if (!load()) {
            icon.state = IconState::Failed;
        }
        return icon.image;
    }

    icon.state = IconState::Progress;
    const pn::Client client{m_server};
    curl::Header header{};
    for (const auto& [key, value] : client.IconHeaders()) {
        header.m_map[key] = value;
    }

    const auto queued = curl::Api().ToFileAsync(
        curl::Url{client.IconUrl(item)},
        curl::Path{path},
        std::move(header),
        curl::StopToken{m_icon_stop.get_token()},
        curl::OnComplete{[this, index](auto& result) {
            // the icon list belongs to the catalog this was asked for, and a new
            // catalog stops this token first, so the index is still that item.
            if (index < m_icons.size()) {
                m_icons[index].state = result.success ? IconState::Done : IconState::Failed;
            }
        }}
    );

    if (!queued) {
        icon.state = IconState::Failed;
    }
    return 0;
}

void Menu::FreeIcons() {
    m_icons.clear();
}

void Menu::ShowMessage(const std::string& message) {
    App::Push<OptionBox>(message, "OK"_i18n);
}

void Menu::DrawCentered(NVGcontext* vg, Theme* theme, const std::string& text) {
    gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 28.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", text.c_str());
}

void Menu::DrawMacs(NVGcontext* vg, Theme* theme) {
    if (m_macs.empty()) {
        DrawCentered(vg, theme, m_discovering ? "Searching the network..."_i18n :
            "No Mac found. Open Ponte on the Mac, then press X to search or Y to type its address."_i18n);
        return;
    }

    constexpr float text_xoffset{15.f};

    m_mac_list->Draw(vg, theme, m_macs.size(), [this](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& row = m_macs[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_mac_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else if (i != static_cast<s64>(m_macs.size()) - 1) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        const auto name = row.server.name.empty() ? row.server.address : row.server.name;
        gfx::drawTextArgs(vg, x + text_xoffset, y + h / 2.f, 22.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), "%s", name.c_str());

        std::string status;
        if (row.saved && !row.server.token.empty()) {
            status = "Paired"_i18n;
        } else if (row.saved) {
            status = "Saved"_i18n;
        } else {
            status = "Not paired"_i18n;
        }
        if (row.found) {
            status += "  •  " + "On the network"_i18n;
        }
        status += "  •  " + row.server.address;

        gfx::drawTextArgs(vg, x + w - text_xoffset, y + h / 2.f, 16.f, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", status.c_str());
    });

    if (m_discovering) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH - 75.f, SCREEN_HEIGHT - 110.f, 18.f, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", "Searching the network..."_i18n.c_str());
    }
}

void Menu::DrawLibrary(NVGcontext* vg, Theme* theme) {
    if (m_rows.empty()) {
        DrawCentered(vg, theme, "Nothing for the Switch on this Mac yet."_i18n);
        return;
    }

    int budget = ICON_BUDGET;

    m_row_list->Draw(vg, theme, m_rows.size(), [this, &budget](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& row = m_rows[i];
        const auto& item = m_catalog.items[row.item];
        const bool chosen = m_chosen[row.item];

        auto text_id = ThemeEntryID_TEXT;
        if (m_row_index == i) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else if (i != static_cast<s64>(m_rows.size()) - 1) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        float cx = x + 15.f + row.depth * CHILD_INDENT;

        // the tick box.
        const Vec4 box{cx, y + (h - 26.f) / 2.f, 26.f, 26.f};
        gfx::drawRect(vg, box, theme->GetColour(chosen ? ThemeEntryID_HIGHLIGHT_1 : ThemeEntryID_GRID), 4.f);
        if (chosen) {
            gfx::drawRect(vg, Vec4{box.x + 7.f, box.y + 7.f, box.w - 14.f, box.h - 14.f}, theme->GetColour(ThemeEntryID_TEXT_SELECTED), 2.f);
        }
        cx += box.w + 14.f;

        // only a game's own row carries an icon; its updates and dlc sit under it.
        if (row.depth == 0) {
            const Vec4 icon_v{cx, y + 6.f, h - 12.f, h - 12.f};
            if (const auto image = GetIcon(row.item, budget)) {
                gfx::drawImage(vg, icon_v, image, 5.f);
            } else {
                gfx::drawRect(vg, icon_v, theme->GetColour(ThemeEntryID_GRID), 5.f);
            }
            cx += icon_v.w + 14.f;
        }

        const auto state = StateLabel(item);
        const float right = x + w - 15.f;

        nvgSave(vg);
        nvgIntersectScissor(vg, cx, y, right - cx - 190.f, h);
            gfx::drawTextArgs(vg, cx, y + h * 0.36f, 21.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), "%s", item.title.c_str());
            gfx::drawTextArgs(vg, cx, y + h * 0.72f, 15.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "%s", DescribeItem(item).c_str());
        nvgRestore(vg);

        gfx::drawTextArgs(vg, right, y + h / 2.f, 17.f, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, StateColour(item, theme), "%s", state.c_str());
    });

    // why the row under the cursor can't be installed, said once at the bottom.
    const auto& current = m_catalog.items[m_rows[m_row_index].item];
    if (current.state == pn::State::Incompatible && !current.state_reason.empty()) {
        gfx::drawTextArgs(vg, 75.f, SCREEN_HEIGHT - 110.f, 18.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_ERROR), "%s", current.state_reason.c_str());
    }
}

void SignalInstalled() {
    g_installs++;
}

} // namespace sphaira::ui::menu::ponte
