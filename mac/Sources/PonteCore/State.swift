import Foundation

/// Cruzamento do catálogo com o que já está no console (SPEC §5.7).
public enum ConsoleState {
    public static func profileWarnings(kind: String, profile: Profile?, guessed: Bool) -> [String] {
        guard let profile else { return [] }
        var out = guessed ? ["profile_guessed"] : []
        switch profile.status(kind).status {
        case "experimental": out.append("experimental_format")
        case "unknown": out.append("unknown_compatibility")
        default: break
        }
        return out
    }

    /// (state, state_reason) na ordem de precedência da SPEC.
    public static func itemState(_ item: Item, installed: [[String: Any]], firmware: String?,
                                 profile: Profile?) -> (state: String, reason: String?) {
        let i = item.ident
        if let profile {
            let (status, note) = profile.status(i.kind)
            if status == "blocked" { return ("incompatible", note ?? "formato não suportado neste firmware") }
        }
        if let req = i.requiredFirmware, let firmware, Versions.compareFirmware(req, firmware) == 1 {
            return ("incompatible", "exige firmware \(req) ou superior")
        }
        guard let tid = i.titleId, !tid.isEmpty else { return ("new", nil) }
        let same = installed.filter {
            textValue($0["title_id"]).uppercased() == tid.uppercased() && ($0["content"] as? String) == i.content
        }
        if same.isEmpty { return ("new", nil) }
        if same.contains(where: { Versions.compareVersions(i.version, optionalText($0["version"]), platform: i.platform) <= 0 }) {
            return ("installed", nil)
        }
        return ("update_available", nil)
    }

    /// Catálogo anotado com `state` e `state_reason`.
    public static func annotate(_ items: [Item], console: [String: Any], profiles: [Profile]) -> [[String: Any]] {
        let platform = console["platform"] as? String
        let firmware = console["firmware"] as? String
        let installed = (console["installed"] as? [Any] ?? []).compactMap { $0 as? [String: Any] }
        let (profile, guessed) = Profiles.select(profiles, platform: platform, firmware: firmware)
        return items.map { it in
            var d = it.summary()
            d["warnings"] = uniqued(it.allWarnings + profileWarnings(kind: it.ident.kind, profile: profile, guessed: guessed))
            let (state, reason) = itemState(it, installed: installed, firmware: firmware, profile: profile)
            d["state"] = state
            d["state_reason"] = orNull(reason)
            return d
        }
    }

    /// Como `str(x)` da referência para os campos de texto do console (números viram texto).
    static func textValue(_ v: Any?) -> String {
        switch v {
        case let s as String: return s
        case let n as NSNumber: return n.stringValue
        default: return ""
        }
    }

    static func optionalText(_ v: Any?) -> String? {
        switch v {
        case let s as String: return s
        case let n as NSNumber: return n.stringValue
        default: return nil
        }
    }
}
