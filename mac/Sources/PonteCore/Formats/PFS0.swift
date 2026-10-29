import Foundation

/// Leitor do contêiner PFS0 (NSP/NSZ do Switch).
///
/// Layout (little-endian):
///   0x00 magic "PFS0"
///   0x04 número de arquivos (u32)
///   0x08 tamanho da tabela de nomes (u32)
///   0x0C reservado
///   0x10 entradas de 0x18 bytes: offset u64, size u64, name_offset u32, reservado u32
///   depois: tabela de nomes (textos terminados em zero)
public enum PFS0 {
    public struct Failure: Error, CustomStringConvertible {
        public let description: String
    }

    public struct Entry: Equatable {
        public let name: String
        public let offset: UInt64
        public let size: UInt64
    }

    static let magic = Array("PFS0".utf8)
    static let entrySize = 0x18
    static let maxFiles = 100_000
    static let maxStrings = 16 << 20

    public static func list(path: String) throws -> [Entry] {
        guard let fh = FileHandle(forReadingAtPath: path) else { throw Failure(description: "não foi possível abrir") }
        defer { try? fh.close() }
        let head = [UInt8](try fh.read(upToCount: 16) ?? Data())
        guard head.count == 16, Array(head[0..<4]) == magic else { throw Failure(description: "não é um PFS0") }
        let count = Int(head.u32le(4)!)
        let strSize = Int(head.u32le(8)!)
        guard count <= maxFiles, strSize <= maxStrings else { throw Failure(description: "cabeçalho PFS0 inválido") }
        let rawEntries = count > 0 ? [UInt8](try fh.read(upToCount: count * entrySize) ?? Data()) : []
        let strings = strSize > 0 ? [UInt8](try fh.read(upToCount: strSize) ?? Data()) : []
        guard rawEntries.count == count * entrySize, strings.count == strSize else {
            throw Failure(description: "PFS0 truncado")
        }
        var out: [Entry] = []
        for i in 0..<count {
            let e = i * entrySize
            let off = rawEntries.u64le(e)!
            let size = rawEntries.u64le(e + 8)!
            let nameOff = Int(rawEntries.u32le(e + 16)!)
            guard let end = strings.indexOfZero(from: nameOff) else { throw Failure(description: "nome fora dos limites") }
            out.append(Entry(name: String(decoding: strings[nameOff..<end], as: UTF8.self), offset: off, size: size))
        }
        return out
    }

    /// Gera um PFS0 sintético. Para testes. `files` em ordem.
    public static func build(_ files: [(String, [UInt8])]) -> [UInt8] {
        var strings: [UInt8] = []
        var nameOffsets: [Int] = []
        for (n, _) in files {
            nameOffsets.append(strings.count)
            strings += Array(n.utf8) + [0]
        }
        while (16 + files.count * entrySize + strings.count) % 0x20 != 0 { strings.append(0) }
        var entries: [UInt8] = []
        var data: [UInt8] = []
        for ((_, content), noff) in zip(files, nameOffsets) {
            entries.appendLE(UInt64(data.count))
            entries.appendLE(UInt64(content.count))
            entries.appendLE(UInt32(noff))
            entries.appendLE(UInt32(0))
            data += content
        }
        var out = magic
        out.appendLE(UInt32(files.count))
        out.appendLE(UInt32(strings.count))
        out.appendLE(UInt32(0))
        return out + entries + strings + data
    }
}
