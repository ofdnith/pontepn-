#pragma once

// "Mac library": the ponte client. lists the macs on the lan, pairs with one,
// shows its library next to what this console already holds, and streams the
// chosen games, updates and dlc straight into yati. see utils/ponte.hpp.

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"
#include "utils/ponte.hpp"
#include "utils/thread.hpp"
#include "fs.hpp"

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace sphaira::ui::menu::ponte {

struct Menu final : MenuBase {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Ponte"; }
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    enum class Mode { Macs, Library };

    // one row of the mac list: saved, found on the network just now, or both.
    struct MacRow {
        sphaira::ponte::Server server{};
        bool saved{};
        bool found{};
    };

    enum class IconState { None, Progress, Done, Failed };

    struct Icon {
        Icon() = default;
        Icon(const Icon&) = delete;
        auto operator=(const Icon&) -> Icon& = delete;
        Icon(Icon&& rhs) noexcept;
        auto operator=(Icon&& rhs) noexcept -> Icon&;
        ~Icon();
        int image{};
        IconState state{IconState::None};
        bool tried_cache{};
    };

    // how a worker's connect ended.
    enum class ConnectOutcome { Connected, NeedCode, Failed };

private:
    void SetMode(Mode mode);
    // marks the buttons for rebinding on the next frame: an Action replaced
    // from inside its own callback would destroy the running std::function.
    void UpdateActions();
    void ApplyActions();
    // Y and '-' belong to MainMenu while this menu is one of its tabs; they are
    // borrowed while bound here and handed back when not.
    void Borrow(Button button, std::optional<Action>& prev, bool& borrowed, const Action& action);
    void GiveBack(Button button, std::optional<Action>& prev, bool& borrowed);
    void UpdateSubheading();

    void LoadSaved();
    void BuildMacRows();
    void StartDiscovery();
    void PromptAddress();
    void ForgetSelected();
    void PromptCode(const sphaira::ponte::Server& server);
    // runs the whole handshake on a worker: info, pairing when a code is given,
    // the console scan and the annotated catalog.
    void Connect(const sphaira::ponte::Server& server, const std::string& code = {});
    // the console scan and the catalog again, for the mac already connected.
    void Refresh(bool quiet = false);
    void ApplyCatalog(sphaira::ponte::Catalog&& catalog, sphaira::ponte::ConsoleState&& state);
    void PollChanges();

    void ToggleRow();
    void SelectAllNew();
    void ClearSelection();
    void InstallChosen();
    // takes copies: the catalog can be replaced by a refresh while the
    // storage popup is up.
    void StartInstall(std::vector<sphaira::ponte::CatalogItem> items, bool sd);

    auto ChosenItems() const -> std::vector<std::size_t>;
    auto FreeOn(const char* storage) const -> s64;
    auto GetIcon(std::size_t item, int& budget) -> int;
    void FreeIcons();
    void ShowMessage(const std::string& message);
    // a build before 0.1.1 could copy itself over /hbmenu.nro on exit; run
    // from there, this offers to put the homebrew menu back.
    void OfferHbmenuRestore();

    void DrawMacs(NVGcontext* vg, Theme* theme);
    void DrawLibrary(NVGcontext* vg, Theme* theme);
    void DrawCentered(NVGcontext* vg, Theme* theme, const std::string& text);

private:
    Mode m_mode{Mode::Macs};
    bool m_actions_dirty{};
    bool m_hbmenu_checked{};
    std::optional<Action> m_prev_y{};
    std::optional<Action> m_prev_select{};
    bool m_borrowed_y{};
    bool m_borrowed_select{};

    std::vector<sphaira::ponte::Server> m_saved{};
    std::vector<sphaira::ponte::Found> m_found{};
    std::vector<MacRow> m_macs{};
    s64 m_mac_index{};
    std::unique_ptr<List> m_mac_list{};
    bool m_discovering{};
    bool m_searched{};

    // one worker at a time does the talking: connect, refresh or a change poll.
    // B while it runs stops it, and the answer is then dropped.
    bool m_busy{};     // the list is replaced by m_busy_text.
    bool m_working{};  // m_work is running, shown or not; cleared by its answer.
    std::string m_busy_text{};
    std::stop_source m_work_stop{};
    std::unique_ptr<utils::Async> m_work{};
    std::unique_ptr<utils::Async> m_discovery{};
    // stopped on the way out, so nothing queued for the main thread runs
    // against a menu that has gone.
    std::stop_source m_life{};

    // the mac this library came from.
    sphaira::ponte::Server m_server{};
    sphaira::ponte::Catalog m_catalog{};
    sphaira::ponte::ConsoleState m_console{};
    std::vector<sphaira::ponte::Row> m_rows{};
    std::vector<u8> m_chosen{}; // one per catalog item.
    std::vector<Icon> m_icons{};  // one per catalog item.
    s64 m_row_index{};
    std::unique_ptr<List> m_row_list{};
    // icon downloads belong to the catalog on screen.
    std::stop_source m_icon_stop{};

    // the mac is asked every so often whether its library grew, on a worker of
    // its own so the list never waits on it.
    TimeStamp m_last_poll{};
    bool m_polling{};
    std::unique_ptr<utils::Async> m_poll{};
    // the SignalInstalled count the catalog on screen was fetched at.
    u32 m_installs_seen{};
};

// an install from anywhere changed what this console holds; the library asks
// the mac again the next time it is shown.
void SignalInstalled();

} // namespace sphaira::ui::menu::ponte
