import Foundation

/// Leitor e gerador de param.sfo (PS4).
///
/// Layout (little-endian):
///   0x00 magic "\0PSF"
///   0x04 versão (u32)
///   0x08 início da tabela de chaves (u32)
///   0x0C início da tabela de dados (u32)
///   0x10 número de entradas (u32)
///   0x14 entradas de 16 bytes: key_offset u16, fmt u16, len u32, max_len u32, data_offset u32
public enum SFO {
    public enum Value: Equatable, Sendable {
        case string(String)
        case int(UInt32)

        public var string: String? { if case .string(let s) = self { return s }; return nil }
        public var int: UInt32? { if case .int(let v) = self { return v }; return nil }
    }

    public struct Failure: Error, CustomStringConvertible {
        public let description: String
    }

    static let magic: [UInt8] = [0x00, 0x50, 0x53, 0x46]
    static let fmtUTF8Special: UInt16 = 0x0004
    static let fmtUTF8: UInt16 = 0x0204
    static let fmtInt32: UInt16 = 0x0404
    static let maxEntries = 1024

    public static func parse(_ data: [UInt8]) throws -> [String: Value] {
        guard data.count >= 20, Array(data[0..<4]) == magic else { throw Failure(description: "não é um param.sfo") }
        let keyStart = Int(data.u32le(8)!)
        let dataStart = Int(data.u32le(12)!)
        let count = Int(data.u32le(16)!)
        guard count <= maxEntries, 20 + count * 16 <= data.count else {
            throw Failure(description: "número de entradas inválido")
        }
        var out: [String: Value] = [:]
        for i in 0..<count {
            let e = 20 + i * 16
            let keyOff = Int(data.u16le(e)!)
            let fmt = data.u16le(e + 2)!
            let length = Int(data.u32le(e + 4)!)
            let dataOff = Int(data.u32le(e + 12)!)
            let k0 = keyStart + keyOff
            guard let k1 = data.indexOfZero(from: k0) else { throw Failure(description: "chave fora dos limites") }
            let key = String(decoding: data[k0..<k1], as: UTF8.self)
            let d0 = dataStart + dataOff
            guard d0 >= 0, d0 + length <= data.count else { throw Failure(description: "dado fora dos limites em \(key)") }
            let raw = Array(data[d0..<(d0 + length)])
            switch fmt {
            case fmtInt32:
                var padded = Array(raw.prefix(4))
                while padded.count < 4 { padded.append(0) }
                out[key] = .int(padded.u32le(0)!)
            case fmtUTF8, fmtUTF8Special:
                out[key] = .string(cString(raw[...]))
            default:
                continue
            }
        }
        return out
    }

    /// Gera um param.sfo válido. Para testes e dados sintéticos.
    public static func build(_ values: [String: Value]) -> [UInt8] {
        let keys = values.keys.sorted()
        var keyTable: [UInt8] = []
        var keyOffsets: [Int] = []
        for k in keys {
            keyOffsets.append(keyTable.count)
            keyTable += Array(k.utf8) + [0]
        }
        while keyTable.count % 4 != 0 { keyTable.append(0) }

        var dataTable: [UInt8] = []
        var entries: [UInt8] = []
        for (k, koff) in zip(keys, keyOffsets) {
            var raw: [UInt8]
            let fmt: UInt16
            let length: Int
            let maxLen: Int
            switch values[k]! {
            case .int(let v):
                raw = []
                raw.appendLE(v)
                fmt = fmtInt32; length = 4; maxLen = 4
            case .string(let s):
                raw = Array(s.utf8) + [0]
                fmt = fmtUTF8; length = raw.count
                maxLen = (length + 3) / 4 * 4
                while raw.count < maxLen { raw.append(0) }
            }
            entries.appendLE(UInt16(koff))
            entries.appendLE(fmt)
            entries.appendLE(UInt32(length))
            entries.appendLE(UInt32(maxLen))
            entries.appendLE(UInt32(dataTable.count))
            dataTable += raw
        }
        let keyStart = 20 + entries.count
        let dataStart = keyStart + keyTable.count
        var out = magic
        out.appendLE(UInt32(0x0101))
        out.appendLE(UInt32(keyStart))
        out.appendLE(UInt32(dataStart))
        out.appendLE(UInt32(keys.count))
        return out + entries + keyTable + dataTable
    }
}
