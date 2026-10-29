import Foundation

/// Leitura de sce_sys/param.json (PS5) — SPEC §1.5.
public enum PS5Param {
    public struct Failure: Error, CustomStringConvertible {
        public let description: String
    }

    public struct Meta: Equatable {
        public var titleId: String?
        public var contentId: String?
        public var version: String?
        public var title: String?
        public var requiredFirmware: String?
    }

    public static func parse(_ data: Data) throws -> Meta {
        guard let obj = JSON.object(data) else { throw Failure(description: "JSON inválido") }
        guard let d = obj as? [String: Any] else { throw Failure(description: "param.json não é um objeto") }

        var title: String?
        if let lp = d["localizedParameters"] as? [String: Any] {
            if let lang = lp["defaultLanguage"] as? String, let entry = lp[lang] as? [String: Any] {
                title = JSON.string(entry["titleName"])
            }
            if title == nil {
                // na falta do idioma padrão, o primeiro idioma com nome (em ordem de chave, para ser estável)
                for key in lp.keys.sorted() {
                    if let entry = lp[key] as? [String: Any], let t = JSON.string(entry["titleName"]) {
                        title = t
                        break
                    }
                }
            }
        }
        return Meta(
            titleId: JSON.string(d["titleId"]),
            contentId: JSON.string(d["contentId"]),
            version: JSON.string(d["contentVersion"]),
            title: title,
            requiredFirmware: Versions.firmwareFromHexText(d["requiredSystemSoftwareVersion"] as? String)
        )
    }
}
