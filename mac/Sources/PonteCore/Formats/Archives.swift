import Foundation

/// Detecção e agrupamento de arquivos compactados (SPEC §1.3).
///
/// Aqui só se reconhece o conjunto de partes e se há buracos na numeração; a extração fica em
/// `Extractor` (app do Mac).
public struct ArchiveSet: Equatable {
    /// Nome do conjunto, ex.: "Jogo.rar".
    public let key: String
    public internal(set) var parts: [String] = []
    public internal(set) var numbers: [Int] = []

    public var missing: [Int] {
        guard !numbers.isEmpty else { return [] }
        let nums = Set(numbers)
        let start = nums.contains(0) ? 0 : 1
        let last = nums.max()!
        return (start...max(start, last)).filter { !nums.contains($0) && $0 <= last }
    }

    public var complete: Bool { missing.isEmpty }

    /// Parte por onde a extração começa (a que o 7-Zip/bsdtar espera receber).
    public var firstPart: String? {
        let names = parts.map(lastComponent)
        let lower = names.map { $0.lowercased() }
        let prefs: [(String) -> Bool] = [
            { RX(#"\.part0*1\.rar$"#).matches($0) },
            { $0.hasSuffix(".001") },
            { $0.hasSuffix(".rar") || $0.hasSuffix(".zip") || $0.hasSuffix(".7z") || $0.hasSuffix(".tar")
                || $0.hasSuffix(".tgz") || $0.hasSuffix(".tar.gz") },
        ]
        for pref in prefs {
            if let i = lower.firstIndex(where: pref) { return parts[i] }
        }
        return parts.first
    }
}

public enum Archives {
    private static let partRar = RX(#"^(.+)\.part(\d+)\.rar$"#, caseInsensitive: true)
    private static let numbered = RX(#"^(.+)\.(7z|zip|rar|tar)\.(\d{3})$"#, caseInsensitive: true)
    private static let rarOld = RX(#"^(.+)\.r(\d{2})$"#, caseInsensitive: true)
    private static let zipOld = RX(#"^(.+)\.z(\d{2})$"#, caseInsensitive: true)
    private static let single = RX(#"^(.+)\.(zip|rar|7z|tar|tgz|tar\.gz)$"#, caseInsensitive: true)

    /// (chave do conjunto, número da parte ou nil) se `name` for compactado.
    public static func match(_ name: String) -> (key: String, part: Int?)? {
        if let m = partRar.search(name), let base = m[1], let n = Int(m[2]!) {
            return ("\(base).rar", n)
        }
        if let m = numbered.search(name), let base = m[1], let n = Int(m[3]!) {
            return ("\(base).\(m[2]!.lowercased())", n)
        }
        if let m = rarOld.search(name), let base = m[1], let n = Int(m[2]!) {
            return ("\(base).rar", n + 1)  // .rar é a parte 0, .r00 a parte 1…
        }
        if let m = zipOld.search(name), let base = m[1], let n = Int(m[2]!) {
            return ("\(base).zip", n)
        }
        if let m = single.search(name), let base = m[1] {
            return ("\(base).\(m[2]!.lowercased())", nil)
        }
        return nil
    }

    /// Agrupa caminhos (absolutos) em conjuntos, por pasta e nome de conjunto.
    public static func group(_ paths: [String]) -> [ArchiveSet] {
        var sets: [String: ArchiveSet] = [:]
        var order: [String] = []
        for p in paths {
            let name = lastComponent(p)
            guard let (key, n) = match(name) else { continue }
            let groupKey = parentPath(p) + "\u{0}" + key.lowercased()
            if sets[groupKey] == nil {
                sets[groupKey] = ArchiveSet(key: key)
                order.append(groupKey)
            }
            sets[groupKey]!.parts.append(p)
            if let n {
                sets[groupKey]!.numbers.append(n)
            } else if pathSuffix(name).lowercased() == ".rar" {
                // rar antigo: "Jogo.rar" é a parte 0 de "Jogo.r00", "Jogo.r01"…
                // Sozinho, é um rar simples; o número 0 só importa se houver .rNN.
                sets[groupKey]!.numbers.append(0)
            }
            // "Jogo.zip" ao lado de "Jogo.z01…" é a última parte do zip dividido:
            // não tem número próprio e não entra na checagem de buracos.
        }
        var out: [ArchiveSet] = []
        for k in order {
            var s = sets[k]!
            s.parts.sort(by: utf8Less)
            if s.numbers == [0] { s.numbers = [] }
            out.append(s)
        }
        return out.enumerated().sorted { a, b in
            let ka = a.element.key.lowercased(), kb = b.element.key.lowercased()
            return ka != kb ? ka < kb : a.offset < b.offset
        }.map(\.element)
    }
}
