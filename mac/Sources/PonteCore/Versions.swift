import Foundation

/// Comparação de versões e firmwares (SPEC §5.7) e conversões BCD (SPEC §1.4, §1.5).
public enum Versions {
    private static func bcdByte(_ b: UInt64) -> Int { Int((b >> 4) * 10 + (b & 0x0F)) }

    /// Firmware em BCD para "X.YY": os dois bytes mais altos guardam versão maior e menor.
    /// 0x05050000 (4 bytes) -> "5.05"; 0x0403000000000000 (8 bytes) -> "4.03".
    public static func firmwareFromBCD(_ value: UInt64, widthBytes: Int) -> String? {
        guard value > 0, widthBytes >= 2, widthBytes <= 8 else { return nil }
        let shift = UInt64((widthBytes - 1) * 8)
        let major = bcdByte((value >> shift) & 0xFF)
        let minor = bcdByte((value >> (shift - 8)) & 0xFF)
        return "\(major)." + (minor < 10 ? "0\(minor)" : "\(minor)")
    }

    /// `requiredSystemSoftwareVersion` do param.json: "0x0403000000000000" -> "4.03".
    public static func firmwareFromHexText(_ text: String?) -> String? {
        guard let text, !text.isEmpty else { return nil }
        var digits = text.lowercased()
        if digits.hasPrefix("0x") { digits.removeFirst(2) }
        guard !digits.isEmpty, let value = UInt64(digits, radix: 16) else { return nil }
        let width = max(1, (digits.count + 1) / 2)
        guard width >= 2 else { return nil }
        return firmwareFromBCD(value, widthBytes: width)
    }

    /// "13.60" -> (13, 60).
    public static func parseFirmware(_ text: String?) -> (Int, Int)? {
        guard let text else { return nil }
        let parts = text.trimmingCharacters(in: .whitespaces).split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 2, let a = digitsValue(parts[0]), let b = digitsValue(parts[1]) else { return nil }
        return (a, b)
    }

    /// -1, 0, 1; nil se algum não for um firmware válido.
    public static func compareFirmware(_ a: String?, _ b: String?) -> Int? {
        guard let pa = parseFirmware(a), let pb = parseFirmware(b) else { return nil }
        return pa == pb ? 0 : (pa < pb ? -1 : 1)
    }

    /// Compara versões de conteúdo. Versão nil (ou ilegível) de qualquer lado conta como igual,
    /// para não sugerir cópias à toa (SPEC §5.7).
    public static func compareVersions(_ a: String?, _ b: String?, platform: String) -> Int {
        guard let a, let b else { return 0 }
        if platform == "switch" {
            guard let ia = digitsValue(Substring(a.trimmingCharacters(in: .whitespaces))),
                  let ib = digitsValue(Substring(b.trimmingCharacters(in: .whitespaces))) else { return 0 }
            return ia == ib ? 0 : (ia < ib ? -1 : 1)
        }
        guard var pa = versionParts(a), var pb = versionParts(b) else { return 0 }
        let n = max(pa.count, pb.count)
        pa += [Int](repeating: 0, count: n - pa.count)
        pb += [Int](repeating: 0, count: n - pb.count)
        if pa == pb { return 0 }
        return pa.lexicographicallyPrecedes(pb) ? -1 : 1
    }

    private static func versionParts(_ text: String) -> [Int]? {
        var out: [Int] = []
        for p in text.trimmingCharacters(in: .whitespaces).split(separator: ".", omittingEmptySubsequences: false) {
            guard let v = digitsValue(p) else { return nil }
            out.append(v)
        }
        return out
    }

    static func digitsValue(_ s: Substring) -> Int? {
        guard !s.isEmpty, s.allSatisfy({ $0.isASCII && $0.isNumber }) else { return nil }
        return Int(s)
    }
}
