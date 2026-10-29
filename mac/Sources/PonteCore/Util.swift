import Foundation
#if canImport(CryptoKit)
import CryptoKit
#endif

// MARK: - Expressões regulares

/// Atalho sobre NSRegularExpression (funciona igual no macOS e no Linux).
public final class RX: @unchecked Sendable {
    let re: NSRegularExpression

    public init(_ pattern: String, caseInsensitive: Bool = false) {
        // swiftlint:disable:next force_try
        re = try! NSRegularExpression(pattern: pattern, options: caseInsensitive ? [.caseInsensitive] : [])
    }

    /// Primeira ocorrência em qualquer lugar do texto. Índice 0 = trecho inteiro; grupos que não
    /// participaram vêm como nil.
    public func search(_ s: String) -> [String?]? {
        let ns = s as NSString
        guard let m = re.firstMatch(in: s, range: NSRange(location: 0, length: ns.length)) else { return nil }
        return (0..<m.numberOfRanges).map { i in
            let r = m.range(at: i)
            return r.location == NSNotFound ? nil : ns.substring(with: r)
        }
    }

    public func matches(_ s: String) -> Bool { search(s) != nil }

    /// Grupo 1 da última ocorrência (ou o trecho inteiro, sem grupos).
    public func lastMatch(_ s: String) -> String? {
        let ns = s as NSString
        guard let m = re.matches(in: s, range: NSRange(location: 0, length: ns.length)).last else { return nil }
        let r = m.range(at: m.numberOfRanges > 1 ? 1 : 0)
        return r.location == NSNotFound ? nil : ns.substring(with: r)
    }

    /// Substitui todas as ocorrências.
    public func replace(_ s: String, with template: String) -> String {
        let ns = s as NSString
        return re.stringByReplacingMatches(in: s, range: NSRange(location: 0, length: ns.length), withTemplate: template)
    }
}

// MARK: - Bytes

extension Array where Element == UInt8 {
    func u16le(_ o: Int) -> UInt16? {
        guard o >= 0, o + 2 <= count else { return nil }
        return UInt16(self[o]) | UInt16(self[o + 1]) << 8
    }

    func u32le(_ o: Int) -> UInt32? {
        guard o >= 0, o + 4 <= count else { return nil }
        var v: UInt32 = 0
        for i in (0..<4).reversed() { v = v << 8 | UInt32(self[o + i]) }
        return v
    }

    func u64le(_ o: Int) -> UInt64? {
        guard o >= 0, o + 8 <= count else { return nil }
        var v: UInt64 = 0
        for i in (0..<8).reversed() { v = v << 8 | UInt64(self[o + i]) }
        return v
    }

    func u32be(_ o: Int) -> UInt32? {
        guard o >= 0, o + 4 <= count else { return nil }
        var v: UInt32 = 0
        for i in 0..<4 { v = v << 8 | UInt32(self[o + i]) }
        return v
    }

    /// Índice do primeiro zero a partir de `from`, ou nil.
    func indexOfZero(from: Int) -> Int? {
        guard from >= 0, from < count else { return nil }
        return self[from...].firstIndex(of: 0)
    }

    mutating func appendLE<T: FixedWidthInteger>(_ v: T) {
        Swift.withUnsafeBytes(of: v.littleEndian) { append(contentsOf: $0) }
    }

    mutating func appendBE<T: FixedWidthInteger>(_ v: T) {
        Swift.withUnsafeBytes(of: v.bigEndian) { append(contentsOf: $0) }
    }

    mutating func putBE(_ v: UInt32, at o: Int) {
        for i in 0..<4 { self[o + i] = UInt8(truncatingIfNeeded: v >> UInt32(24 - 8 * i)) }
    }
}

/// Texto até o primeiro zero, em UTF-8 com substituição de bytes inválidos.
func cString(_ bytes: ArraySlice<UInt8>) -> String {
    let end = bytes.firstIndex(of: 0) ?? bytes.endIndex
    return String(decoding: bytes[bytes.startIndex..<end], as: UTF8.self)
}

public func hexString<S: Sequence>(_ bytes: S) -> String where S.Element == UInt8 {
    let digits = Array("0123456789abcdef".utf8)
    var out = [UInt8]()
    for b in bytes {
        out.append(digits[Int(b >> 4)])
        out.append(digits[Int(b & 0x0F)])
    }
    return String(decoding: out, as: UTF8.self)
}

// MARK: - SHA-256

/// SHA-256 incremental. Usa o CryptoKit no macOS; no Linux (onde os testes também rodam), uma
/// implementação própria.
public struct Hasher256 {
    #if canImport(CryptoKit)
    private var inner = SHA256()
    public init() {}
    public mutating func update(_ data: Data) { inner.update(data: data) }
    public mutating func update(_ bytes: [UInt8]) { inner.update(data: bytes) }
    public func hex() -> String { hexString(inner.finalize()) }
    #else
    private var inner = PortableSHA256()
    public init() {}
    public mutating func update(_ data: Data) { inner.update([UInt8](data)) }
    public mutating func update(_ bytes: [UInt8]) { inner.update(bytes) }
    public func hex() -> String { hexString(inner.finalized()) }
    #endif

    public static func hex(_ text: String) -> String {
        var h = Hasher256()
        h.update(Array(text.utf8))
        return h.hex()
    }
}

/// SHA-256 em Swift puro (FIPS 180-4).
struct PortableSHA256 {
    private static let k: [UInt32] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ]
    private var h: [UInt32] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
    private var buffer: [UInt8] = []
    private var length: UInt64 = 0

    mutating func update(_ bytes: [UInt8]) {
        length &+= UInt64(bytes.count)
        var input = bytes[...]
        if !buffer.isEmpty {
            let need = 64 - buffer.count
            buffer.append(contentsOf: input.prefix(need))
            input = input.dropFirst(need)
            if buffer.count < 64 { return }
            compress(buffer[...])
            buffer.removeAll(keepingCapacity: true)
        }
        while input.count >= 64 {
            compress(input.prefix(64))
            input = input.dropFirst(64)
        }
        buffer.append(contentsOf: input)
    }

    func finalized() -> [UInt8] {
        var copy = self
        let bitLength = length &* 8
        var pad: [UInt8] = [0x80]
        let rem = (buffer.count + 1) % 64
        pad.append(contentsOf: [UInt8](repeating: 0, count: rem <= 56 ? 56 - rem : 120 - rem))
        pad.appendBE(bitLength)
        let saved = copy.length
        copy.update(pad)
        copy.length = saved
        var out: [UInt8] = []
        for v in copy.h { out.appendBE(v) }
        return out
    }

    private mutating func compress(_ block: ArraySlice<UInt8>) {
        var w = [UInt32](repeating: 0, count: 64)
        let base = block.startIndex
        for i in 0..<16 {
            w[i] = UInt32(block[base + 4 * i]) << 24 | UInt32(block[base + 4 * i + 1]) << 16
                | UInt32(block[base + 4 * i + 2]) << 8 | UInt32(block[base + 4 * i + 3])
        }
        func rotr(_ x: UInt32, _ n: UInt32) -> UInt32 { x >> n | x << (32 - n) }
        for i in 16..<64 {
            let s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)
            let s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10)
            w[i] = w[i - 16] &+ s0 &+ w[i - 7] &+ s1
        }
        var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7]
        for i in 0..<64 {
            let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)
            let ch = (e & f) ^ (~e & g)
            let t1 = hh &+ s1 &+ ch &+ PortableSHA256.k[i] &+ w[i]
            let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)
            let maj = (a & b) ^ (a & c) ^ (b & c)
            let t2 = s0 &+ maj
            hh = g; g = f; f = e; e = d &+ t1; d = c; c = b; b = a; a = t1 &+ t2
        }
        h[0] &+= a; h[1] &+= b; h[2] &+= c; h[3] &+= d
        h[4] &+= e; h[5] &+= f; h[6] &+= g; h[7] &+= hh
    }
}

// MARK: - Arquivos

public struct FileStat: Equatable {
    public let size: Int64
    public let mtimeNs: Int64
    public let isDirectory: Bool
    public let isRegular: Bool

    public var mtime: Double { Double(mtimeNs) / 1_000_000_000 }
}

/// `stat` seguindo links simbólicos, como `Path.stat()` na referência.
public func fileStat(_ path: String) -> FileStat? {
    var st = stat()
    guard stat(path, &st) == 0 else { return nil }
    #if os(Linux)
    let ns = Int64(st.st_mtim.tv_sec) * 1_000_000_000 + Int64(st.st_mtim.tv_nsec)
    #else
    let ns = Int64(st.st_mtimespec.tv_sec) * 1_000_000_000 + Int64(st.st_mtimespec.tv_nsec)
    #endif
    let fmt = st.st_mode & S_IFMT
    return FileStat(size: Int64(st.st_size), mtimeNs: ns, isDirectory: fmt == S_IFDIR, isRegular: fmt == S_IFREG)
}

public func isDirectory(_ path: String) -> Bool { fileStat(path)?.isDirectory ?? false }
public func isFile(_ path: String) -> Bool { fileStat(path)?.isRegular ?? false }

/// Junta caminhos com "/", sem normalizar.
public func joinPath(_ base: String, _ rel: String) -> String {
    if base.hasSuffix("/") { return base + rel }
    return base + "/" + rel
}

public func lastComponent(_ path: String) -> String {
    (path as NSString).lastPathComponent
}

public func parentPath(_ path: String) -> String {
    (path as NSString).deletingLastPathComponent
}

/// Extensão como o `Path.suffix` do Python: do último ponto em diante, desde que o ponto não seja
/// o primeiro nem o último caractere.
func pathSuffix(_ name: String) -> String {
    guard let dot = name.lastIndex(of: "."), dot != name.startIndex,
          name.index(after: dot) != name.endIndex else { return "" }
    return String(name[dot...])
}

func readFileBytes(_ path: String) -> [UInt8]? {
    guard let data = FileManager.default.contents(atPath: path) else { return nil }
    return [UInt8](data)
}

/// Comparação por bytes UTF-8 (ordem dos `files` na SPEC §5.4).
func utf8Less(_ a: String, _ b: String) -> Bool {
    a.utf8.lexicographicallyPrecedes(b.utf8)
}

// MARK: - JSON e datas

public enum JSON {
    /// JSON compacto, com chaves ordenadas e sem escapar "/".
    public static func data(_ object: Any, pretty: Bool = false) -> Data {
        var opts: JSONSerialization.WritingOptions = [.sortedKeys, .withoutEscapingSlashes]
        if pretty { opts.insert(.prettyPrinted) }
        return (try? JSONSerialization.data(withJSONObject: object, options: opts)) ?? Data("{}".utf8)
    }

    public static func object(_ data: Data) -> Any? {
        var d = data
        if d.starts(with: [0xEF, 0xBB, 0xBF]) { d = d.dropFirst(3) }
        return try? JSONSerialization.jsonObject(with: d, options: [.fragmentsAllowed])
    }

    /// Valor de texto não vazio, ou nil.
    static func string(_ v: Any?) -> String? {
        guard let s = v as? String, !s.isEmpty else { return nil }
        return s
    }
}

/// `nil` vira `NSNull()` (null no JSON).
func orNull(_ v: Any?) -> Any { v ?? NSNull() }

public enum ISOTime {
    public static func format(_ ts: Double) -> String {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        f.timeZone = TimeZone(identifier: "UTC")
        return f.string(from: Date(timeIntervalSince1970: floor(ts)))
    }

    /// Só aceita `AAAA-MM-DDTHH:MM:SSZ`.
    public static func parse(_ text: String?) -> Double? {
        guard let text, RX(#"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$"#).matches(text) else { return nil }
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f.date(from: text)?.timeIntervalSince1970
    }
}

/// Velocidade dos últimos segundos a partir de um contador de bytes que só cresce.
/// Usado em tudo que mostra "MB/s": cópias, extração, conversão e hashes.
public struct RateMeter: Sendable {
    public static let window: TimeInterval = 5
    private var samples: [(Date, Int64)] = []

    public init() {}

    /// Registra o total acumulado agora.
    public mutating func add(total: Int64, at now: Date = Date()) {
        if let last = samples.last, now.timeIntervalSince(last.0) < 0.2 {
            samples[samples.count - 1] = (last.0, total)
        } else {
            samples.append((now, total))
        }
        samples.removeAll { now.timeIntervalSince($0.0) > RateMeter.window + 1 }
    }

    /// Bytes por segundo na janela; 0 quando não há dados suficientes.
    public func bytesPerSecond(at now: Date = Date()) -> Double {
        guard let first = samples.first(where: { now.timeIntervalSince($0.0) <= RateMeter.window }),
              let last = samples.last else { return 0 }
        // parado há mais de 2 s: velocidade zero, não a média antiga
        if now.timeIntervalSince(last.0) > 2 { return 0 }
        let span = max(now.timeIntervalSince(first.0), 1)
        return Double(max(0, last.1 - first.1)) / span
    }

    public mutating func reset() { samples.removeAll() }
}
