import Foundation

/// Arquivo de um item, relativo à raiz do item (SPEC §5.4).
public struct FileEntry: Equatable, Sendable {
    public let index: Int
    /// Relativo à raiz do item, com "/".
    public let path: String
    public let absPath: String
    public let size: Int64
    public let mtimeNs: Int64

    var cacheKey: String { "\(absPath)\u{0}\(size)\u{0}\(mtimeNs)" }
}

/// Item pronto do catálogo. Imutável depois da varredura que o criou.
public final class Item: @unchecked Sendable {
    public let id: String
    public let revision: String
    /// Caminho relativo à biblioteca.
    public let rel: String
    public let absPath: String
    public let ident: Identified
    public let files: [FileEntry]
    public let addedTS: Double
    public internal(set) var warnings: [String] = []

    init(id: String, revision: String, rel: String, absPath: String, ident: Identified, files: [FileEntry], addedTS: Double) {
        self.id = id
        self.revision = revision
        self.rel = rel
        self.absPath = absPath
        self.ident = ident
        self.files = files
        self.addedTS = addedTS
    }

    public var size: Int64 { files.reduce(0) { $0 + $1.size } }
    public var platform: String { ident.platform }
    public var title: String { (ident.title?.isEmpty == false ? ident.title : nil) ?? lastComponent(absPath) }

    public var hasIcon: Bool {
        ident.iconData != nil || (ident.iconPath.map(isFile) ?? false)
    }

    public func iconBytes() -> Data? {
        if let d = ident.iconData { return d }
        if let p = ident.iconPath { return FileManager.default.contents(atPath: p) }
        return nil
    }

    /// Avisos sem repetição, na ordem em que apareceram.
    public var allWarnings: [String] { uniqued(ident.warnings + warnings) }

    /// Campos de catálogo (SPEC §5.3).
    public func summary() -> [String: Any] {
        let i = ident
        return [
            "id": id,
            "revision": revision,
            "platform": i.platform,
            "kind": i.kind,
            "content": i.content,
            "title": title,
            "title_id": orNull(i.titleId),
            "content_id": orNull(i.contentId),
            "version": orNull(i.version),
            "required_firmware": orNull(i.requiredFirmware),
            "size": size,
            "file_count": files.count,
            "parent_title_id": orNull(i.parentTitleId),
            "warnings": allWarnings,
            "added_at": ISOTime.format(addedTS),
            "icon": hasIcon ? "/api/v1/items/\(id)/icon" as Any : NSNull(),
        ]
    }
}

/// Sem repetição, mantendo a ordem.
public func uniqued(_ xs: [String]) -> [String] {
    var seen = Set<String>()
    return xs.filter { seen.insert($0).inserted }
}

/// Biblioteca do Mac: varredura, estabilidade, itens, duplicados e hashes (SPEC §5.3, §5.4, §7).
public final class Library: @unchecked Sendable {
    public static let downloadMarkers = [".part", ".crdownload", ".download", ".tmp"]
    public static let skipDirs: Set<String> = ["_originais"]
    public static let maxDepth = 2
    static let hashChunk = 4 << 20

    public let root: String
    public var stableSeconds: Double
    public var ps5Prefixes: [String]
    let clock: () -> Double

    private let lock = NSRecursiveLock()
    private var itemsById: [String: Item] = [:]
    private var _attention: [NeedsAttention] = []
    private var _archives: [ArchiveSet] = []
    private var _pending: [String] = []
    private var _toConvert: [(path: String, ident: Identified)] = []
    private var _generatedAt: String
    private var hashCache: [String: String] = [:]
    private let hashCacheFile: String?
    private var hashCacheDirty = false

    /// `hashCacheFile`: onde guardar os sha256 já calculados, para não recalcular jogos inteiros a
    /// cada vez que o app abre. nil = só em memória.
    public init(root: String, stableSeconds: Double = 10, ps5Prefixes: [String] = PKG.defaultPS5Prefixes,
                hashCacheFile: String? = nil, clock: @escaping () -> Double = { Date().timeIntervalSince1970 }) {
        self.root = root.count > 1 && root.hasSuffix("/") ? String(root.dropLast()) : root
        self.stableSeconds = stableSeconds
        self.ps5Prefixes = ps5Prefixes
        self.clock = clock
        self.hashCacheFile = hashCacheFile
        _generatedAt = ISOTime.format(clock())
        if let f = hashCacheFile, let data = FileManager.default.contents(atPath: f),
           let obj = JSON.object(data) as? [String: String] {
            hashCache = obj
        }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    public var attention: [NeedsAttention] { locked { _attention } }
    public var archives: [ArchiveSet] { locked { _archives } }
    public var pending: [String] { locked { _pending } }
    /// Pastas de PS5 estáveis que esperam a conversão para .exfat (com `holdFolders`).
    public var toConvert: [(path: String, ident: Identified)] { locked { _toConvert } }

    /// Segurar pastas de PS5 fora do catálogo até virarem .exfat (SPEC §7.4).
    public var holdFolders: Bool {
        get { locked { _holdFolders } }
        set { locked { _holdFolders = newValue } }
    }
    private var _holdFolders = false
    public var generatedAt: String { locked { _generatedAt } }

    // MARK: varredura

    public func scan() {
        var items: [String: Item] = [:]
        var attention: [NeedsAttention] = []
        var archivePaths: [String] = []
        var pending: [String] = []
        var toConvert: [(path: String, ident: Identified)] = []
        let hold = holdFolders
        let now = clock()

        func visit(_ dir: String, _ depth: Int) {
            guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir) else { return }
            for name in names.sorted(by: utf8Less) {
                if name.hasPrefix(".") || Identifier.isAuxiliary(name) || Library.skipDirs.contains(name) { continue }
                let p = joinPath(dir, name)
                guard let st = fileStat(p) else { continue }
                if st.isRegular && Archives.match(name) != nil {
                    archivePaths.append(p)
                    continue
                }
                if st.isRegular && Library.downloadMarkers.contains(where: { name.lowercased().hasSuffix($0) }) { continue }
                guard let result = Identifier.identify(p, ps5Prefixes: ps5Prefixes) else {
                    if st.isDirectory && depth < Library.maxDepth { visit(p, depth + 1) }
                    continue
                }
                if !isStable(p, now: now) {
                    pending.append(p)
                    continue
                }
                switch result {
                case .attention(let a):
                    attention.append(a)
                case .identified(let ident):
                    if hold && ident.kind == "folder" {
                        toConvert.append((p, ident))
                    } else if let item = makeItem(p, ident) {
                        items[item.id] = item
                    }
                }
            }
        }

        visit(root, 1)
        Library.markDuplicates(items)
        let archives = Archives.group(archivePaths)
        locked {
            let changed = Set(items.keys) != Set(itemsById.keys)
                || items.contains { itemsById[$0.key]?.revision != $0.value.revision }
            itemsById = items
            _attention = attention
            _archives = archives
            _pending = pending
            _toConvert = toConvert
            if changed { _generatedAt = ISOTime.format(now) }
        }
    }

    /// Arquivos que o Finder cria sozinho (.DS_Store, ._*): nunca fazem parte de um item.
    public static func isFinderJunk(_ name: String) -> Bool {
        name == ".DS_Store" || name.hasPrefix("._")
    }

    /// (relativo, absoluto, stat) de cada arquivo do item, em ordem de bytes UTF-8.
    /// Pastas apontadas por links simbólicos não são percorridas (como `os.walk`).
    public static func listFiles(_ path: String) throws -> [(String, String, FileStat)] {
        guard let st = fileStat(path) else { throw CocoaError(.fileReadNoSuchFile) }
        if st.isRegular { return [(lastComponent(path), path, st)] }
        var out: [(String, String, FileStat)] = []
        guard let walker = FileManager.default.enumerator(atPath: path) else { throw CocoaError(.fileReadUnknown) }
        while let rel = walker.nextObject() as? String {
            let name = lastComponent(rel)
            if isFinderJunk(name) { continue }
            let abs = joinPath(path, rel)
            guard let fst = fileStat(abs) else { throw CocoaError(.fileReadNoSuchFile) }
            if fst.isRegular { out.append((rel, abs, fst)) }
        }
        out.sort { utf8Less($0.0, $1.0) }
        return out
    }

    static func hasDownloadMarker(_ path: String) -> Bool {
        if isFile(path) {
            return downloadMarkers.contains { FileManager.default.fileExists(atPath: path + $0) }
        }
        guard let walker = FileManager.default.enumerator(atPath: path) else { return false }
        while let rel = walker.nextObject() as? String {
            let lower = lastComponent(rel).lowercased()
            if downloadMarkers.contains(where: { lower.hasSuffix($0) }) { return true }
        }
        return false
    }

    public func isStable(_ path: String, now: Double) -> Bool {
        if Library.hasDownloadMarker(path) { return false }
        if stableSeconds <= 0 { return true }
        guard let files = try? Library.listFiles(path) else { return false }
        let newest = files.map { $0.2.mtime }.max() ?? fileStat(path)?.mtime ?? now
        return now - newest >= stableSeconds
    }

    public static func itemId(platform: String, rel: String) -> String {
        String(Hasher256.hex("\(platform):\(rel)").prefix(16))
    }

    static func revision(_ files: [FileEntry]) -> String {
        var h = Hasher256()
        for f in files { h.update(Array("\(f.path)\u{0}\(f.size)\u{0}\(f.mtimeNs)\n".utf8)) }
        return String(h.hex().prefix(8))
    }

    public func relativePath(_ path: String) -> String {
        let prefix = root.hasSuffix("/") ? root : root + "/"
        return path.hasPrefix(prefix) ? String(path.dropFirst(prefix.count)) : lastComponent(path)
    }

    func makeItem(_ path: String, _ ident: Identified) -> Item? {
        guard let listed = try? Library.listFiles(path) else { return nil }
        var files: [FileEntry] = []
        let isDir = isDirectory(path)
        for (rel, abs, st) in listed {
            if isDir && Identifier.isAuxiliary(lastComponent(abs)) { continue }
            files.append(FileEntry(index: files.count, path: rel, absPath: abs, size: st.size, mtimeNs: st.mtimeNs))
        }
        let rel = relativePath(path)
        let added = listed.map { $0.2.mtime }.max() ?? fileStat(path)?.mtime ?? clock()
        return Item(id: Library.itemId(platform: ident.platform, rel: rel), revision: Library.revision(files),
                    rel: rel, absPath: path, ident: ident, files: files, addedTS: added)
    }

    static func markDuplicates(_ items: [String: Item]) {
        var groups: [String: [Item]] = [:]
        for it in items.values {
            let i = it.ident
            guard let tid = i.titleId, !tid.isEmpty else { continue }
            let key = [i.platform, tid, i.content, i.version ?? "\u{1}nil"].joined(separator: "\u{0}")
            groups[key, default: []].append(it)
        }
        for group in groups.values where group.count > 1 {
            let sorted = group.sorted { a, b in
                a.addedTS != b.addedTS ? a.addedTS < b.addedTS : utf8Less(a.rel, b.rel)
            }
            for it in sorted.dropFirst() { it.warnings.append("duplicate_title_id") }
        }
    }

    // MARK: consultas

    /// Itens prontos, por título (sem diferenciar maiúsculas).
    public func items(platform: String? = nil) -> [Item] {
        let all = locked { Array(itemsById.values) }
        return all.filter { platform == nil || $0.platform == platform }.sorted { a, b in
            let ta = (a.ident.title ?? "").lowercased(), tb = (b.ident.title ?? "").lowercased()
            return ta != tb ? ta < tb : utf8Less(a.rel, b.rel)
        }
    }

    public func get(_ id: String) -> Item? { locked { itemsById[id] } }

    /// Ids de itens que existem mas ainda não estão prontos (para 409).
    public func pendingIds() -> Set<String> {
        var out = Set<String>()
        for p in pending {
            if case .identified(let i) = Identifier.identify(p, ps5Prefixes: ps5Prefixes) {
                out.insert(Library.itemId(platform: i.platform, rel: relativePath(p)))
            }
        }
        for c in toConvert { out.insert(Library.itemId(platform: c.ident.platform, rel: relativePath(c.path))) }
        return out
    }

    // MARK: hashes

    /// Conferência por SHA-256 (SPEC §7.5). Desligada, o Mac não calcula hashes nem os informa
    /// aos consoles, que conferem só o tamanho: cópias começam na hora e não releem nada.
    public var hashesEnabled: Bool {
        get { locked { _hashesEnabled } }
        set { locked { _hashesEnabled = newValue } }
    }
    private var _hashesEnabled = true

    public func sha256(_ f: FileEntry) -> String? { locked { _hashesEnabled ? hashCache[f.cacheKey] : nil } }

    /// Campos de §5.3 mais a lista de arquivos (§5.4).
    public func detail(_ item: Item) -> [String: Any] {
        var d = item.summary()
        d["files"] = item.files.map { f -> [String: Any] in
            ["index": f.index, "path": f.path, "size": f.size, "sha256": orNull(sha256(f))]
        }
        return d
    }

    /// Para testes: força um hash (errado, por exemplo).
    public func setHashForTesting(_ f: FileEntry, _ value: String) { locked { hashCache[f.cacheKey] = value } }

    /// Cálculo de hashes em andamento (para mostrar progresso e velocidade).
    public struct HashProgress: Sendable, Equatable {
        public let title: String
        public let filesLeft: Int
        public let doneBytes: Int64
        public let totalBytes: Int64
        public let bytesPerSecond: Double

        public var fraction: Double { totalBytes > 0 ? min(1, Double(doneBytes) / Double(totalBytes)) : 0 }
        public var secondsLeft: Double? {
            guard bytesPerSecond >= 1, doneBytes < totalBytes else { return nil }
            return Double(totalBytes - doneBytes) / bytesPerSecond
        }
    }

    private var _hashing: (title: String, filesLeft: Int, done: Int64, total: Int64)?
    private var hashMeter = RateMeter()

    /// nil = nada sendo calculado agora.
    public var hashing: HashProgress? {
        locked {
            guard let h = _hashing else { return nil }
            return HashProgress(title: h.title, filesLeft: h.filesLeft, doneBytes: h.done, totalBytes: h.total,
                                bytesPerSecond: hashMeter.bytesPerSecond())
        }
    }

    /// Calcula sha256 dos arquivos que ainda não têm. Retorna quantos calculou.
    @discardableResult
    public func hashPending(shouldStop: () -> Bool = { false }) -> Int {
        guard hashesEnabled else { return 0 }
        var done = 0
        defer {
            saveHashCache()
            locked {
                _hashing = nil
                hashMeter.reset()
            }
        }
        let pending = items().flatMap { it in it.files.filter { f in locked { hashCache[f.cacheKey] } == nil }.map { (it.title, $0) } }
        guard !pending.isEmpty else { return 0 }
        let total = pending.reduce(Int64(0)) { $0 + $1.1.size }
        var before: Int64 = 0
        for (n, (title, f)) in pending.enumerated() {
            if shouldStop() { return done }
            locked { _hashing = (title, pending.count - n, before, total) }
            let start = before
            let hex = Library.hashFile(f.absPath, shouldStop: shouldStop) { read in
                self.locked {
                    self._hashing?.done = start + read
                    self.hashMeter.add(total: start + read)
                }
            }
            before += f.size
            guard let hex else { continue }
            // mudou durante o cálculo: fica para a próxima varredura
            guard let st = fileStat(f.absPath), st.size == f.size, st.mtimeNs == f.mtimeNs else { continue }
            locked {
                hashCache[f.cacheKey] = hex
                hashCacheDirty = true
            }
            done += 1
        }
        return done
    }

    public static func hashFile(_ path: String, shouldStop: () -> Bool = { false },
                                progress: (Int64) -> Void = { _ in }) -> String? {
        guard let fh = FileHandle(forReadingAtPath: path) else { return nil }
        defer { try? fh.close() }
        var h = Hasher256()
        var read: Int64 = 0
        while true {
            if shouldStop() { return nil }
            var chunk = Data()
            let ok: Bool = autoreleasepoolCompat {
                // nil (no Linux) ou vazio = fim do arquivo; erro de leitura = desiste
                do { chunk = try fh.read(upToCount: hashChunk) ?? Data() } catch { return false }
                return true
            }
            guard ok else { return nil }
            if chunk.isEmpty { break }
            h.update(chunk)
            read += Int64(chunk.count)
            progress(read)
        }
        return h.hex()
    }

    func saveHashCache() {
        guard let file = hashCacheFile else { return }
        let snapshot: [String: String]? = locked {
            guard hashCacheDirty else { return nil }
            hashCacheDirty = false
            // só o que ainda existe na biblioteca, para o arquivo não crescer para sempre
            let live = Set(itemsById.values.flatMap { $0.files.map(\.cacheKey) })
            hashCache = hashCache.filter { live.contains($0.key) }
            return hashCache
        }
        guard let snapshot else { return }
        try? JSON.data(snapshot).write(to: URL(fileURLWithPath: file), options: .atomic)
    }
}

/// `autoreleasepool` no macOS (evita acumular memória ao ler arquivos de dezenas de GB); no Linux,
/// só executa o bloco.
func autoreleasepoolCompat<T>(_ body: () -> T) -> T {
    #if canImport(ObjectiveC)
    return autoreleasepool(invoking: body)
    #else
    return body()
    #endif
}
