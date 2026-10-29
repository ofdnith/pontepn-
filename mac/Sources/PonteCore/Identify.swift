import Foundation

/// Item identificado: plataforma, tipo e metadados (SPEC §1).
public struct Identified: Equatable {
    public var platform: String
    public var kind: String
    public var content: String = "unknown"
    public var title: String?
    public var titleId: String?
    public var contentId: String?
    public var version: String?
    public var requiredFirmware: String?
    public var parentTitleId: String?
    public var warnings: [String] = []
    public var iconData: Data?
    public var iconPath: String?

    public init(platform: String, kind: String, content: String = "unknown", title: String? = nil,
                titleId: String? = nil, contentId: String? = nil, version: String? = nil,
                requiredFirmware: String? = nil, parentTitleId: String? = nil, warnings: [String] = [],
                iconData: Data? = nil, iconPath: String? = nil) {
        self.platform = platform
        self.kind = kind
        self.content = content
        self.title = title
        self.titleId = titleId
        self.contentId = contentId
        self.version = version
        self.requiredFirmware = requiredFirmware
        self.parentTitleId = parentTitleId
        self.warnings = warnings
        self.iconData = iconData
        self.iconPath = iconPath
    }
}

/// Parece um jogo, mas não dá para identificar com segurança: você escolhe à mão.
public struct NeedsAttention: Equatable {
    public let path: String
    public let reason: String
}

public enum Identification: Equatable {
    case identified(Identified)
    case attention(NeedsAttention)

    public var identified: Identified? {
        if case .identified(let i) = self { return i }
        return nil
    }
}

/// Identificador de formatos (SPEC §1 e §7.2).
///
/// `identify(path)` olha um arquivo ou pasta da biblioteca e devolve `.identified`,
/// `.attention`, ou nil quando não é um item (arquivo qualquer, pasta comum). Compactados são
/// tratados à parte (`Archives`), porque um conjunto tem várias partes.
public enum Identifier {
    public static let ps5ImageKinds = [".exfat": "exfat", ".ffpkg": "ffpkg", ".ffpfs": "ffpfs", ".ffpfsc": "ffpfsc"]
    public static let switchKinds = [".nsp": "nsp", ".nsz": "nsz", ".xci": "xci", ".xcz": "xcz"]
    public static let kinds: [String: Set<String>] = [
        "ps5": ["folder", "exfat", "ffpkg", "ffpfs", "ffpfsc", "pkg_ps4", "pkg_ps5", "folder_ps4"],
        "switch": ["nsp", "nsz", "xci", "xcz"],
    ]
    public static let contents: Set<String> = ["base", "update", "dlc", "unknown"]
    static let sfoCategory = ["gd": "base", "gp": "update", "ac": "dlc"]
    static let psTitleIdRE = RX(#"(?<![A-Z0-9])([A-Z]{4}\d{5})(?!\d)"#)

    public static let sidecarSuffix = ".ponte.json"
    public static let sidecarIconSuffix = ".ponte.png"
    public static let tmpSuffix = ".ponte-tmp"
    public static let sidecarFields = ["platform", "kind", "content", "title", "title_id", "content_id",
                                       "version", "required_firmware", "parent_title_id"]

    public static func sidecarPath(_ item: String) -> String { item + sidecarSuffix }
    public static func sidecarIconPath(_ item: String) -> String { item + sidecarIconSuffix }

    /// Arquivos do próprio Ponte, que nunca são itens.
    public static func isAuxiliary(_ name: String) -> Bool {
        name.hasSuffix(sidecarSuffix) || name.hasSuffix(sidecarIconSuffix) || name.hasSuffix(tmpSuffix)
    }

    // MARK: arquivo lateral (SPEC §1.6)

    static func readSidecar(_ item: String) -> [String: Any]? {
        let sp = sidecarPath(item)
        guard isFile(sp), let data = FileManager.default.contents(atPath: sp),
              let obj = JSON.object(data) as? [String: Any] else { return nil }
        return obj.filter { sidecarFields.contains($0.key) }
    }

    static func applySidecar(_ item: String, _ auto: Identification?) -> Identification? {
        let iconPath = sidecarIconPath(item)
        guard let side = readSidecar(item) else {
            if case .identified(var i) = auto, isFile(iconPath), i.iconPath == nil, i.iconData == nil {
                i.iconPath = iconPath
                return .identified(i)
            }
            return auto
        }

        var result: Identified
        if case .identified(let i) = auto {
            result = i
        } else {
            guard let platform = side["platform"] as? String, let kind = side["kind"] as? String,
                  kinds[platform]?.contains(kind) == true else {
                return auto ?? .attention(NeedsAttention(path: item, reason: "arquivo .ponte.json sem platform/kind válidos"))
            }
            result = Identified(platform: platform, kind: kind)
        }

        let sidePlatform = side["platform"]
        for key in sidecarFields {
            guard let raw = side[key] else { continue }
            // como na referência: texto ou null; outros tipos são ignorados
            let value: String?
            if let s = raw as? String { value = s } else if raw is NSNull { value = nil } else { continue }
            switch key {
            case "platform":
                guard let v = value, kinds[v] != nil else { continue }
                result.platform = v
            case "kind":
                let plat = sidePlatform == nil ? result.platform : (sidePlatform as? String ?? "")
                guard let v = value, kinds[plat]?.contains(v) == true else { continue }
                result.kind = v
            case "content":
                guard let v = value, contents.contains(v) else { continue }
                result.content = v
            case "title": result.title = value
            case "title_id": result.titleId = value
            case "content_id": result.contentId = value
            case "version": result.version = value
            case "required_firmware": result.requiredFirmware = value
            case "parent_title_id": result.parentTitleId = value
            default: break
            }
        }
        // metadados vieram de uma fonte confiável: o aviso de "só pelo nome" não vale mais
        result.warnings.removeAll { $0 == "metadata_from_filename" }
        if isFile(iconPath) {
            result.iconPath = iconPath
            result.iconData = nil
        }
        return .identified(result)
    }

    // MARK: PS4/PS5

    struct SfoFields {
        var content: String
        var title: String?
        var titleId: String?
        var contentId: String?
        var version: String?
        var requiredFirmware: String?
    }

    static func sfoFields(_ sfo: [String: SFO.Value]) -> SfoFields {
        func s(_ k: String) -> String? {
            guard let v = sfo[k]?.string, !v.isEmpty else { return nil }
            return v
        }
        let category = (sfo["CATEGORY"]?.string ?? "").trimmingCharacters(in: .whitespaces)
        let content = sfoCategory[category] ?? "unknown"
        var fw: String?
        if let sys = sfo["SYSTEM_VER"]?.int { fw = Versions.firmwareFromBCD(UInt64(sys), widthBytes: 4) }
        let version = (content == "base" ? s("APP_VER") : s("VERSION")) ?? s("APP_VER") ?? s("VERSION")
        return SfoFields(content: content, title: s("TITLE"), titleId: s("TITLE_ID"), contentId: s("CONTENT_ID"),
                         version: version, requiredFirmware: fw)
    }

    static func identifyFolder(_ path: String) -> Identification? {
        let pj = joinPath(path, "sce_sys/param.json")
        let ps = joinPath(path, "sce_sys/param.sfo")
        let icon = joinPath(path, "sce_sys/icon0.png")
        let iconPath = isFile(icon) ? icon : nil
        if isFile(pj) {
            guard isFile(joinPath(path, "eboot.bin")) else {
                return .attention(NeedsAttention(path: path, reason: "pasta de PS5 sem eboot.bin na raiz"))
            }
            let meta: PS5Param.Meta
            do {
                guard let data = FileManager.default.contents(atPath: pj) else {
                    throw PS5Param.Failure(description: "não foi possível ler")
                }
                meta = try PS5Param.parse(data)
            } catch {
                return .attention(NeedsAttention(path: path, reason: "param.json ilegível: \(error)"))
            }
            return .identified(Identified(
                platform: "ps5", kind: "folder", content: "base", title: meta.title ?? lastComponent(path),
                titleId: meta.titleId, contentId: meta.contentId, version: meta.version,
                requiredFirmware: meta.requiredFirmware, iconPath: iconPath))
        }
        if isFile(ps) {
            let f: SfoFields
            do {
                guard let data = readFileBytes(ps) else { throw SFO.Failure(description: "não foi possível ler") }
                f = sfoFields(try SFO.parse(data))
            } catch {
                return .attention(NeedsAttention(path: path, reason: "param.sfo ilegível: \(error)"))
            }
            let parent = (f.content == "update" || f.content == "dlc") ? f.titleId : nil
            return .identified(Identified(
                platform: "ps5", kind: "folder_ps4", content: f.content, title: f.title ?? lastComponent(path),
                titleId: f.titleId, contentId: f.contentId, version: f.version,
                requiredFirmware: f.requiredFirmware, parentTitleId: parent, iconPath: iconPath))
        }
        return nil
    }

    static func identifyPS5Image(_ path: String, kind: String) -> Identification {
        let name = lastComponent(path)
        let stem = String(name.dropLast(pathSuffix(name).count))
        let tid = psTitleIdRE.search(stem.uppercased())?[1]
        return .identified(Identified(platform: "ps5", kind: kind, content: "base", title: stem, titleId: tid,
                                      warnings: ["metadata_from_filename"]))
    }

    static func identifyPkg(_ path: String, ps5Prefixes: [String]) -> Identification {
        let info: PKG.Info
        do {
            info = try PKG.read(path: path, ps5Prefixes: ps5Prefixes)
        } catch {
            return .attention(NeedsAttention(path: path, reason: ".pkg não reconhecido: \(error)"))
        }
        let name = lastComponent(path)
        let stem = String(name.dropLast(pathSuffix(name).count))
        var result = Identified(platform: "ps5", kind: info.kind, title: stem, titleId: info.titleId,
                                contentId: info.contentId, iconData: info.icon)
        if let sfo = info.sfo {
            let f = sfoFields(sfo)
            result.content = f.content
            result.title = f.title ?? stem
            result.version = f.version
            result.requiredFirmware = f.requiredFirmware
            if f.content == "update" || f.content == "dlc" { result.parentTitleId = info.titleId }
        } else {
            result.warnings.append("metadata_from_filename")
        }
        return .identified(result)
    }

    static func identifySwitch(_ path: String, kind: String) -> Identification {
        let name = lastComponent(path)
        let stem = String(name.dropLast(pathSuffix(name).count))
        var tid: String?
        var warnings: [String] = []
        if kind == "nsp" || kind == "nsz", let entries = try? PFS0.list(path: path) {
            let tids = SwitchTID.tidsFromTickets(entries.map(\.name))
            if tids.count == 1 { tid = tids.first }
        }
        if tid == nil {
            tid = SwitchTID.tidFromName(name)
            warnings.append("metadata_from_filename")
        }
        let (content, parent) = tid.map(SwitchTID.classify) ?? ("unknown", nil)
        return .identified(Identified(
            platform: "switch", kind: kind, content: content, title: SwitchTID.cleanTitle(stem), titleId: tid,
            version: SwitchTID.versionFromName(name), parentTitleId: parent, warnings: warnings))
    }

    // MARK: entrada

    public static func identify(_ path: String, ps5Prefixes: [String] = PKG.defaultPS5Prefixes) -> Identification? {
        let name = lastComponent(path)
        if isAuxiliary(name) { return nil }
        guard let st = fileStat(path) else { return nil }
        var auto: Identification?
        if st.isDirectory {
            auto = identifyFolder(path)
        } else if st.isRegular {
            let ext = pathSuffix(name).lowercased()
            if let k = ps5ImageKinds[ext] {
                auto = identifyPS5Image(path, kind: k)
            } else if ext == ".pkg" {
                auto = identifyPkg(path, ps5Prefixes: ps5Prefixes)
            } else if let k = switchKinds[ext] {
                auto = identifySwitch(path, kind: k)
            }
        } else {
            return nil
        }
        if auto == nil && !isFile(sidecarPath(path)) { return nil }
        var result = applySidecar(path, auto)
        if case .identified(var i) = result, (i.title ?? "").isEmpty {
            i.title = name
            result = .identified(i)
        }
        return result
    }

    /// Conteúdo do .ponte.json para um item identificado (SPEC §1.6).
    public static func sidecar(_ i: Identified) -> [String: Any] {
        [
            "platform": i.platform, "kind": i.kind, "content": i.content, "title": orNull(i.title),
            "title_id": orNull(i.titleId), "content_id": orNull(i.contentId), "version": orNull(i.version),
            "required_firmware": orNull(i.requiredFirmware), "parent_title_id": orNull(i.parentTitleId),
        ]
    }

    /// Grava o arquivo lateral (escrita atômica).
    public static func writeSidecar(_ i: Identified, for item: String) throws {
        try JSON.data(sidecar(i), pretty: true).write(to: URL(fileURLWithPath: sidecarPath(item)), options: .atomic)
    }
}
