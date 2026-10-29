/*
* Notes:
* - nca's that use title key encryption are decrypted using Tegra SE, whereas
*   standard crypto uses software decryption.
*   The latter is almost always (slightly) faster, and removed the need for es patch.
*/

#pragma once

#include "fs.hpp"
#include "source/base.hpp"
#include "container/base.hpp"
#include "ui/progress_box.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sphaira::yati {

struct Config {
    bool sd_card_install{};

    // enables downgrading patch / data patch (dlc) version.
    bool allow_downgrade{};

    // ignores the install if already installed.
    // checks that every nca is available.
    bool skip_if_already_installed{};

    // installs tickets only.
    bool ticket_only{};

    // flags to enable / disable install of specific types.
    bool skip_base{};
    bool skip_patch{};
    bool skip_addon{};
    bool skip_data_patch{};
    bool skip_ticket{};

    // enables the option to skip sha256 verification.
    bool skip_nca_hash_verify{};

    // enables the option to skip rsa nca fixed key verification.
    bool skip_rsa_header_fixed_key_verify{};

    // enables the option to skip rsa npdm fixed key verification.
    bool skip_rsa_npdm_fixed_key_verify{};

    // if set, it will ignore the distribution bit in the nca header.
    bool ignore_distribution_bit{};

    // converts a personalised ticket to common.
    bool convert_to_common_ticket{};

    // converts titlekey to standard crypto, also known as "ticketless".
    // this will not work with addon (dlc), so, addon tickets will be installed.
    bool convert_to_standard_crypto{};

    // encrypts the keak with master key 0, this allows the game to be launched on every fw.
    // implicitly performs standard crypto.
    bool lower_master_key{};

    // sets the system_firmware field in the cnmt extended header.
    // if mkey is higher than fw version, the game still won't launch
    // as the fw won't have the key to decrypt keak.
    bool lower_system_version{};

    // if not empty, only content whose id is listed is installed.
    std::vector<u64> title_ids{};
};

// Ponte: o que a instalação fez com cada conteúdo (cnmt) do arquivo.
struct Outcome {
    u32 installed{};
    u32 skipped{};
    // motivo do último conteúdo pulado, em inglês (vai pela i18n na tela).
    std::string skip_reason{};
};

// overridable options, set to avoid
struct ConfigOverride {
    std::optional<bool> sd_card_install{};
    std::optional<bool> skip_nca_hash_verify{};
    std::optional<bool> skip_rsa_header_fixed_key_verify{};
    std::optional<bool> skip_rsa_npdm_fixed_key_verify{};
    std::optional<bool> ignore_distribution_bit{};
    std::optional<bool> convert_to_common_ticket{};
    std::optional<bool> convert_to_standard_crypto{};
    std::optional<bool> lower_master_key{};
    std::optional<bool> lower_system_version{};

    // installs only the content whose id is listed, skipping the rest of the
    // container. on a random access source a skipped content costs one cnmt nca.
    std::vector<u64> title_ids{};

    // Ponte: o Mac já decidiu pelo que o console diz ter instalado (registros
    // do ns). um registro velho no ncm, sem jogo de verdade, não deve fazer o
    // yati pular a instalação em silêncio.
    std::optional<bool> skip_if_already_installed{};

    // Ponte: preenchido no fim, para quem chamou saber se algo foi instalado.
    // um conteúdo pulado (já instalado, versão menor, opção "pular base"...)
    // termina com sucesso no yati, mas não instalou nada.
    Outcome* outcome{};
};

Result InstallFromFile(ui::ProgressBox* pbox, fs::Fs* fs, const fs::FsPath& path, const ConfigOverride& override = {});
Result InstallFromSource(ui::ProgressBox* pbox, source::Base* source, const fs::FsPath& path, const ConfigOverride& override = {}, s64 source_size = -1);
Result InstallFromContainer(ui::ProgressBox* pbox, container::Base* container, const ConfigOverride& override = {});
Result InstallFromCollections(ui::ProgressBox* pbox, source::Base* source, const container::Collections& collections, const ConfigOverride& override = {});

} // namespace sphaira::yati
