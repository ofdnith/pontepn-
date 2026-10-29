import Foundation

/// Title ID do Switch: tipo de conteúdo e jogo base (SPEC §1.2).
public enum SwitchTID {
    static let tidInNameRE = RX(#"\[([0-9A-Fa-f]{16})\]"#)
    static let versionInNameRE = RX(#"\[v(\d+)\]"#, caseInsensitive: true)
    static let tikRE = RX(#"^([0-9A-Fa-f]{16})[0-9A-Fa-f]{16}\.tik$"#)
    static let bracketsRE = RX(#"\[[^\]]*\]"#)
    static let spacesRE = RX(#"\s+"#)

    public static func format(_ value: UInt64) -> String {
        let hex = String(value, radix: 16, uppercase: true)
        return String(repeating: "0", count: max(0, 16 - hex.count)) + hex
    }

    /// (content, parent_title_id).
    public static func classify(_ tid: String) -> (content: String, parent: String?) {
        guard let t = UInt64(tid, radix: 16) else { return ("unknown", nil) }
        let low = t & 0xFFF
        let bit12 = t & 0x1000 != 0
        if low == 0 && !bit12 { return ("base", nil) }
        if low == 0x800 { return ("update", format(t & ~UInt64(0xFFF))) }
        if bit12 && low != 0 { return ("dlc", format((t & ~UInt64(0xFFF)) - 0x1000)) }
        return ("unknown", nil)
    }

    public static func tidFromName(_ name: String) -> String? {
        tidInNameRE.search(name)?[1]?.uppercased()
    }

    public static func versionFromName(_ name: String) -> String? {
        guard let v = versionInNameRE.search(name)?[1] else { return nil }
        // "[v065536]" -> "65536"; números enormes ficam como estão, sem zeros à esquerda
        if let n = Int(v) { return String(n) }
        let trimmed = v.drop(while: { $0 == "0" })
        return trimmed.isEmpty ? "0" : String(trimmed)
    }

    public static func tidsFromTickets(_ names: [String]) -> Set<String> {
        Set(names.compactMap { tikRE.search($0)?[1]?.uppercased() })
    }

    /// "Jogo Legal [0100...][v0] (BR)" -> "Jogo Legal (BR)".
    public static func cleanTitle(_ stem: String) -> String {
        var t = bracketsRE.replace(stem, with: "")
        t = spacesRE.replace(t, with: " ")
        t = t.trimmingCharacters(in: CharacterSet(charactersIn: " -_."))
        return t.isEmpty ? stem : t
    }
}
