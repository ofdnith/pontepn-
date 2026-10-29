import Foundation

/// Perfil de compatibilidade por firmware (SPEC §1.1.1).
public struct Profile: Equatable, Sendable {
    public static let statuses: Set<String> = ["ok", "experimental", "unknown", "blocked"]

    public struct KindStatus: Equatable, Sendable {
        public let status: String
        public let note: String?
    }

    public let platform: String
    public let firmware: String
    public let description: String?
    public let kinds: [String: KindStatus]

    public init(platform: String, firmware: String, description: String? = nil, kinds: [String: KindStatus]) {
        self.platform = platform
        self.firmware = firmware
        self.description = description
        self.kinds = kinds
    }

    /// Status do `kind`; ausente ou inválido vale `unknown`.
    public func status(_ kind: String) -> (status: String, note: String?) {
        guard let k = kinds[kind] else { return ("unknown", nil) }
        return (k.status, k.note)
    }

    static func parse(_ object: Any) -> Profile? {
        guard let d = object as? [String: Any], let fw = d["firmware"] as? String,
              Versions.parseFirmware(fw) != nil else { return nil }
        var kinds: [String: KindStatus] = [:]
        for (k, v) in d["kinds"] as? [String: Any] ?? [:] {
            guard let e = v as? [String: Any], let s = e["status"] as? String, statuses.contains(s) else { continue }
            kinds[k] = KindStatus(status: s, note: e["note"] as? String)
        }
        return Profile(platform: d["platform"] as? String ?? "", firmware: fw,
                       description: d["description"] as? String, kinds: kinds)
    }
}

public enum Profiles {
    /// Lê `*.json` da pasta, em ordem de nome. Arquivos ilegíveis são ignorados.
    public static func load(directory: String) -> [Profile] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: directory)) ?? []
        return names.filter { $0.hasSuffix(".json") }.sorted().compactMap { name in
            guard let data = FileManager.default.contents(atPath: joinPath(directory, name)),
                  let obj = JSON.object(data) else { return nil }
            return Profile.parse(obj)
        }
    }

    /// Perfil de maior firmware <= firmware do console. Sem perfil menor ou igual, usa o de menor
    /// firmware e marca como adivinhado. Firmware desconhecido -> (nil, false).
    public static func select(_ profiles: [Profile], platform: String?, firmware: String?) -> (profile: Profile?, guessed: Bool) {
        guard let fw = Versions.parseFirmware(firmware) else { return (nil, false) }
        let candidates = profiles.filter { $0.platform == platform }
            .map { ($0, Versions.parseFirmware($0.firmware)!) }
        guard !candidates.isEmpty else { return (nil, false) }
        // ordenação estável por firmware
        let sorted = candidates.enumerated().sorted { a, b in
            a.element.1 != b.element.1 ? a.element.1 < b.element.1 : a.offset < b.offset
        }.map(\.element)
        if let below = sorted.last(where: { $0.1 <= fw }) { return (below.0, false) }
        return (sorted[0].0, true)
    }
}

extension Profiles {
    /// Cópia dos perfis de `profiles/` do repositório, para semear a pasta de perfis do app na
    /// primeira execução. Um teste confere que é igual ao arquivo do repositório.
    public static let builtin: [String: String] = [
        "ps5-13.60.json": #"""
{
  "platform": "ps5",
  "firmware": "13.60",
  "description": "kstuff + ShadowMountPlus + etaHEN, homebrew habilitado a cada boot",
  "kinds": {
    "exfat":      {"status": "ok"},
    "ffpkg":      {"status": "ok"},
    "folder":     {"status": "ok", "note": "alguns títulos só rodam a partir de USB"},
    "ffpfs":      {"status": "experimental", "note": "suporte experimental no ShadowMountPlus"},
    "ffpfsc":     {"status": "experimental", "note": "suporte experimental no ShadowMountPlus"},
    "pkg_ps4":    {"status": "ok"},
    "pkg_ps5":    {"status": "blocked", "note": "pacote .pkg de PS5 não roda neste firmware"},
    "folder_ps4": {"status": "unknown", "note": "carregador para pastas de PS4 a confirmar"}
  }
}
"""#,
    ]

    /// Grava os perfis embutidos que ainda não existem na pasta. Não sobrescreve os seus.
    public static func seed(directory: String) {
        try? FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        for (name, text) in builtin {
            let path = joinPath(directory, name)
            if !FileManager.default.fileExists(atPath: path) {
                try? Data((text + "\n").utf8).write(to: URL(fileURLWithPath: path))
            }
        }
    }
}
