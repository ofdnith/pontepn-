import Foundation

/// Leitor de cabeçalho de .pkg (PS4/PS5) — SPEC §1.4.
///
/// Campos do cabeçalho em big-endian:
///   0x00 magic 7F 43 4E 54
///   0x10 entry_count (u32)
///   0x18 table_offset (u32)
///   0x40 content_id (36 bytes ASCII)
/// Entradas da tabela: 32 bytes cada; id (+0), offset (+16), size (+20).
public enum PKG {
    public struct Failure: Error, CustomStringConvertible {
        public let description: String
    }

    public struct Info {
        public let contentId: String
        public let titleId: String
        public let kind: String  // "pkg_ps4" | "pkg_ps5"
        public let sfo: [String: SFO.Value]?
        public let icon: Data?
    }

    static let magic: [UInt8] = [0x7F, 0x43, 0x4E, 0x54]
    static let entryParamSFO: UInt32 = 0x1000
    static let entryIcon0: UInt32 = 0x1200
    static let entrySize = 32
    static let maxEntries = 10_000
    static let maxSFO = 1 << 20
    static let maxIcon = 4 << 20

    public static let defaultPS5Prefixes = ["PPSA", "PPSB"]
    static let contentIdRE = RX(#"^[A-Z]{2}\d{4}-([A-Z]{4}\d{5})_\d{2}-[A-Z0-9]{16}$"#)

    public static func read(path: String, ps5Prefixes: [String] = defaultPS5Prefixes) throws -> Info {
        guard let st = fileStat(path), let fh = FileHandle(forReadingAtPath: path) else {
            throw Failure(description: "não foi possível abrir o arquivo")
        }
        defer { try? fh.close() }
        return try read(handle: fh, fileSize: UInt64(st.size), ps5Prefixes: ps5Prefixes)
    }

    static func readAt(_ fh: FileHandle, _ offset: UInt64, _ count: Int) throws -> [UInt8] {
        try fh.seek(toOffset: offset)
        return [UInt8](try fh.read(upToCount: count) ?? Data())
    }

    public static func read(handle fh: FileHandle, fileSize: UInt64, ps5Prefixes: [String] = defaultPS5Prefixes) throws -> Info {
        let header = try readAt(fh, 0, 0x80)
        guard header.count >= 0x64 else { throw Failure(description: "arquivo curto demais para um .pkg") }
        guard Array(header[0..<4]) == magic else { throw Failure(description: "magic desconhecido") }

        let rawCid = header[0x40..<(0x40 + 36)]
        let end = rawCid.firstIndex(of: 0) ?? rawCid.endIndex
        let contentId = String(decoding: rawCid[rawCid.startIndex..<end], as: UTF8.self)
        guard let m = contentIdRE.search(contentId), let titleId = m[1] else {
            throw Failure(description: "content id inválido: \(contentId)")
        }
        let kind = ps5Prefixes.contains(String(titleId.prefix(4))) ? "pkg_ps5" : "pkg_ps4"

        let entryCount = Int(header.u32be(0x10)!)
        let tableOffset = UInt64(header.u32be(0x18)!)
        var sfo: [String: SFO.Value]?
        var icon: Data?
        if entryCount > 0, entryCount <= maxEntries, tableOffset + UInt64(entryCount * entrySize) <= fileSize {
            let table = try readAt(fh, tableOffset, entryCount * entrySize)
            if table.count == entryCount * entrySize {
                for i in 0..<entryCount {
                    let eid = table.u32be(i * entrySize)!
                    let off = UInt64(table.u32be(i * entrySize + 16)!)
                    let size = Int(table.u32be(i * entrySize + 20)!)
                    if off + UInt64(size) > fileSize { continue }
                    if eid == entryParamSFO, size > 0, size <= maxSFO, sfo == nil {
                        sfo = try? SFO.parse(try readAt(fh, off, size))
                    } else if eid == entryIcon0, size > 0, size <= maxIcon, icon == nil {
                        icon = Data(try readAt(fh, off, size))
                    }
                }
            }
        }
        return Info(contentId: contentId, titleId: titleId, kind: kind, sfo: sfo, icon: icon)
    }

    /// Gera um .pkg sintético com cabeçalho, tabela de entradas e dados. Para testes.
    public static func build(contentId: String, sfo: [UInt8]? = nil, icon: [UInt8]? = nil, body: [UInt8] = []) -> [UInt8] {
        var entries: [(UInt32, [UInt8])] = []
        if let sfo { entries.append((entryParamSFO, sfo)) }
        if let icon { entries.append((entryIcon0, icon)) }
        let tableOffset = 0x1000
        let dataOffset = tableOffset + entries.count * entrySize
        var table: [UInt8] = []
        var data: [UInt8] = []
        for (eid, blob) in entries {
            for v in [eid, 0, 0, 0, UInt32(dataOffset + data.count), UInt32(blob.count)] { table.appendBE(v) }
            table += [UInt8](repeating: 0, count: 8)
            data += blob
        }
        var header = [UInt8](repeating: 0, count: tableOffset)
        header.replaceSubrange(0..<4, with: magic)
        header.putBE(UInt32(entries.count), at: 0x10)
        header.putBE(UInt32(tableOffset), at: 0x18)
        let cid = Array(contentId.utf8.prefix(36))
        header.replaceSubrange(0x40..<(0x40 + cid.count), with: cid)
        return header + table + data + body
    }
}
