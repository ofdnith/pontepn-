import Foundation
import PonteCore

/// Preparação de itens no Mac: extrair compactados (SPEC §1.3) e converter pastas de PS5 em
/// .exfat (SPEC §7.4). Um trabalho por vez, numa thread própria, para não travar as varreduras.
public final class Preparer: @unchecked Sendable {
    public enum Stage: String, Sendable {
        case extracting = "extraindo"
        case converting = "convertendo"
    }

    public struct Job: Sendable, Equatable {
        public let key: String
        public let title: String
        public let stage: Stage
        /// 0…1 quando conhecido.
        public var progress: Double?
        public var step: String
        /// Bytes já gravados (extraídos ou copiados para a imagem).
        public var doneBytes: Int64 = 0
        /// Total esperado; nil quando não dá para saber antes (ex.: tamanho descompactado).
        public var totalBytes: Int64?
        public var bytesPerSecond: Double = 0
        /// Esperando: um console está copiando e o preparo cedeu a vez.
        public var paused = false
        public var startedAt = Date()

        public init(key: String, title: String, stage: Stage, progress: Double?, step: String) {
            self.key = key
            self.title = title
            self.stage = stage
            self.progress = progress
            self.step = step
        }

        /// Segundos até terminar: pelos bytes quando o total é conhecido, senão pelo percentual.
        public var secondsLeft: Double? {
            if paused { return nil }
            if let total = totalBytes, bytesPerSecond >= 1, doneBytes < total {
                return Double(total - doneBytes) / bytesPerSecond
            }
            if let p = progress, p > 0.02, p < 1 {
                let elapsed = Date().timeIntervalSince(startedAt)
                return elapsed * (1 - p) / p
            }
            return nil
        }
    }

    /// Item que precisa de você (senha, falha, conjunto incompleto…).
    public struct Problem: Sendable, Equatable {
        public let key: String
        public let title: String
        public let reason: String
        public let needsPassword: Bool
    }

    /// Ferramentas do macOS usadas na conversão (trocáveis nos testes).
    public struct Tools: Sendable {
        public var mkfile = "/usr/sbin/mkfile"
        public var hdiutil = "/usr/bin/hdiutil"
        public var newfsExfat = "/sbin/newfs_exfat"
        public var diskutil = "/usr/sbin/diskutil"
        public var mount = "/sbin/mount"
        public var umount = "/sbin/umount"
    }

    public var tools = Tools()
    public var convertFolders = false
    public var extractArchives = false
    public var keepOriginals = true
    public var force64K = false
    /// true = ceder a vez agora (um console está copiando). O trabalho pausa e continua sozinho.
    public var shouldYield: (() -> Bool)?

    private let library: Library
    private let log: (String) -> Void
    private let lock = NSLock()
    private var _current: Job?
    private var _problems: [String: Problem] = [:]
    private var passwords: [String: String] = [:]
    private var cancelled = false
    private var runningProcess: Process?
    private var meter = RateMeter()

    public init(library: Library, log: @escaping (String) -> Void) {
        self.library = library
        self.log = log
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    public var current: Job? { locked { _current } }
    public var problems: [Problem] { locked { _problems.values.sorted { $0.title.lowercased() < $1.title.lowercased() } } }

    /// Informa a senha de um compactado e tenta de novo.
    public func setPassword(_ password: String, for key: String) {
        locked {
            passwords[key] = password
            _problems[key] = nil
        }
    }

    /// Esquece a falha e tenta de novo na próxima varredura.
    public func retry(_ key: String) { locked { _problems[key] = nil } }

    public func cancel() {
        locked {
            cancelled = true
            runningProcess?.terminate()
        }
    }

    private var isCancelled: Bool { locked { cancelled } }

    /// Desfaz `cancel()`: volta a começar trabalhos nas próximas varreduras.
    public func resume() { locked { cancelled = false } }

    /// Espera o trabalho atual terminar (depois de `cancel()`, ele para e limpa o que criou).
    public func waitUntilIdle(timeout: TimeInterval) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while locked({ _current != nil }) {
            if Date() >= deadline { return false }
            usleep(50_000)
        }
        return true
    }

    private func setProblem(_ key: String, _ title: String, _ reason: String, password: Bool = false) {
        locked { _problems[key] = Problem(key: key, title: title, reason: reason, needsPassword: password) }
        log("[preparo] \(title): \(reason)")
    }

    private func update(progress: Double? = nil, step: String? = nil, done: Int64? = nil, total: Int64? = nil,
                        paused: Bool? = nil) {
        locked {
            guard var j = _current else { return }
            if let progress { j.progress = progress }
            if let step { j.step = step }
            if let total { j.totalBytes = total }
            if let done {
                j.doneBytes = done
                meter.add(total: done)
            }
            if let paused { j.paused = paused }
            j.bytesPerSecond = j.paused ? 0 : meter.bytesPerSecond()
            _current = j
        }
    }

    private var mustYield: Bool { shouldYield?() ?? false }

    /// Se um console estiver copiando, espera ele terminar (o trabalho aparece como pausado).
    /// false = cancelado enquanto esperava.
    private func waitWhileCopying() -> Bool {
        guard mustYield else { return !isCancelled }
        update(paused: true)
        while mustYield && !isCancelled { usleep(500_000) }
        update(paused: false)
        return !isCancelled
    }

    // MARK: escolha do próximo trabalho

    /// Chamado depois de cada varredura. Se não houver trabalho em andamento, começa o próximo.
    public func tick(shouldStop: @escaping () -> Bool = { false }) {
        if locked({ _current != nil || cancelled }) { return }
        let now = Date().timeIntervalSince1970

        // conjuntos incompletos aparecem como problema, sem trava (somem quando a parte chegar)
        if extractArchives {
            var incomplete: [String: Problem] = [:]
            for set in library.archives where !set.complete {
                let key = Preparer.archiveKey(set)
                let missing = set.missing.prefix(5).map(String.init).joined(separator: ", ")
                incomplete[key] = Problem(key: key, title: set.key, reason: "conjunto incompleto: faltam as partes \(missing)",
                                          needsPassword: false)
            }
            locked {
                _problems = _problems.filter { !$0.value.reason.hasPrefix("conjunto incompleto") }
                    .merging(incomplete) { a, _ in a }
            }
        }

        if extractArchives {
            for set in library.archives where set.complete {
                let key = Preparer.archiveKey(set)
                if locked({ _problems[key] != nil }) { continue }
                guard set.parts.allSatisfy({ library.isStable($0, now: now) }) else { continue }
                startJob(Job(key: key, title: set.key, stage: .extracting, progress: nil, step: "extraindo")) {
                    self.extract(set, key: key)
                }
                return
            }
        }
        if convertFolders {
            for (path, ident) in library.toConvert {
                if locked({ _problems[path] != nil }) { continue }
                let title = ident.title ?? lastComponent(path)
                startJob(Job(key: path, title: title, stage: .converting, progress: 0, step: "calculando")) {
                    self.convert(path, ident)
                }
                return
            }
        }
    }

    private func startJob(_ job: Job, _ work: @escaping () -> Void) {
        locked {
            _current = job
            meter.reset()
        }
        log("[preparo] \(job.stage.rawValue): \(job.title)")
        let t = Thread { [weak self] in
            work()
            // varre antes de liberar: quem espera o fim (ex.: para ejetar o disco) não pode ter
            // uma varredura ainda lendo a biblioteca
            if let self, !self.isCancelled { self.library.scan() }
            self?.locked { self?._current = nil }
        }
        t.name = "ponte-preparo"
        t.qualityOfService = .utility
        t.start()
    }

    static func archiveKey(_ set: ArchiveSet) -> String {
        joinPath(parentPath(set.parts.first ?? ""), set.key)
    }

    // MARK: extração (SPEC §1.3)

    static let percentRE = RX(#"(\d{1,3})%"#)

    static let toolSearchPaths = ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"]

    static func findTool(_ names: [String]) -> String? {
        for dir in toolSearchPaths {
            for n in names {
                let p = joinPath(dir, n)
                if FileManager.default.isExecutableFile(atPath: p) { return p }
            }
        }
        return nil
    }

    /// Nome do conjunto sem a extensão: "Jogo.rar" -> "Jogo", "Jogo.tar.gz" -> "Jogo".
    static func baseName(_ key: String) -> String {
        for ext in [".tar.gz", ".tgz", ".tar", ".rar", ".zip", ".7z"] where key.lowercased().hasSuffix(ext) {
            return String(key.dropLast(ext.count))
        }
        return key
    }

    /// Comando de extração para o conjunto, ou o motivo de não haver.
    static func extractionCommand(_ set: ArchiveSet, dest: String, password: String?) -> (tool: String, args: [String])? {
        guard let first = set.firstPart else { return nil }
        let lower = set.key.lowercased()
        let multi = set.parts.count > 1
        let isTar = lower.hasSuffix(".tar") || lower.hasSuffix(".tgz") || lower.hasSuffix(".tar.gz")
        if let sz = findTool(["7zz", "7z", "7za"]), !(isTar && !multi) {
            // senha fictícia quando não há senha: o 7-Zip nunca para esperando digitação
            // -bsp1: percentual na saída padrão, para a barra de progresso
            return (sz, ["x", "-y", "-bso0", "-bsp1", "-o" + dest, "-p" + (password ?? "ponte-sem-senha"), first])
        }
        if !multi, password == nil, let tar = findTool(["bsdtar"]) {
            return (tar, ["-x", "-f", first, "-C", dest])
        }
        return nil
    }

    func extract(_ set: ArchiveSet, key: String) {
        let parent = parentPath(set.parts.first ?? "")
        let base = Preparer.baseName(set.key)
        let tmp = joinPath(parent, base + Identifier.tmpSuffix)
        let fm = FileManager.default
        try? fm.removeItem(atPath: tmp)
        do {
            try fm.createDirectory(atPath: tmp, withIntermediateDirectories: true)
        } catch {
            return setProblem(key, set.key, "não foi possível criar a pasta temporária: \(error.localizedDescription)")
        }
        let password = locked { passwords[key] }
        guard let (tool, args) = Preparer.extractionCommand(set, dest: tmp, password: password) else {
            try? fm.removeItem(atPath: tmp)
            let why = set.parts.count > 1 || password != nil
                ? "precisa do 7-Zip para partes divididas ou senha (brew install sevenzip)"
                : "nenhum extrator encontrado (instale o 7-Zip: brew install sevenzip)"
            return setProblem(key, set.key, why)
        }
        guard waitWhileCopying() else { try? fm.removeItem(atPath: tmp); return }
        // tamanho descompactado só é conhecido de antemão num .tar simples
        let lowerKey = set.key.lowercased()
        let archiveBytes = set.parts.reduce(Int64(0)) { $0 + (fileStat($1)?.size ?? 0) }
        let knownTotal: Int64? = lowerKey.hasSuffix(".tar") && set.parts.count == 1 ? archiveBytes : nil
        update(step: "extraindo", done: 0, total: knownTotal)
        var lastMeasure = Date.distantPast
        let (status, output) = run(tool, args, onOutput: { text in
            // o 7-Zip escreve "  37% 12 - pasta/arquivo" (com retrocessos); vale o último percentual
            if let m = Preparer.percentRE.lastMatch(text), let p = Double(m) {
                self.update(progress: min(1, p / 100))
            }
        }, onTick: {
            guard Date().timeIntervalSince(lastMeasure) >= 1 else { return }
            lastMeasure = Date()
            let written = Preparer.directorySize(tmp)
            var progress: Double?
            if let knownTotal, knownTotal > 0 { progress = min(0.99, Double(written) / Double(knownTotal)) }
            self.update(progress: progress, done: written)
        })
        if isCancelled { try? fm.removeItem(atPath: tmp); return }
        guard status == 0 else {
            try? fm.removeItem(atPath: tmp)
            let lower = output.lowercased()
            if lower.contains("wrong password") || lower.contains("password") || lower.contains("encrypted") {
                return setProblem(key, set.key, password == nil ? "senha necessária" : "senha errada", password: true)
            }
            let tail = output.split(separator: "\n").suffix(3).joined(separator: " ")
            return setProblem(key, set.key, "falha ao extrair (código \(status)): \(tail)")
        }
        // achata: se saiu um único item de dentro, ele vai direto para a pasta do compactado,
        // para não passar do limite de 2 níveis da varredura (SPEC §7.2)
        let entries = ((try? fm.contentsOfDirectory(atPath: tmp)) ?? []).filter { !Library.isFinderJunk($0) && $0 != "__MACOSX" }
        do {
            if entries.count == 1 {
                let target = Preparer.uniquePath(joinPath(parent, entries[0]))
                try fm.moveItem(atPath: joinPath(tmp, entries[0]), toPath: target)
                try? fm.removeItem(atPath: tmp)
            } else {
                try fm.moveItem(atPath: tmp, toPath: Preparer.uniquePath(joinPath(parent, base)))
            }
        } catch {
            try? fm.removeItem(atPath: tmp)
            return setProblem(key, set.key, "não foi possível mover o conteúdo extraído: \(error.localizedDescription)")
        }
        disposeOriginals(set.parts)
        locked { passwords[key] = nil }
        log("[preparo] extraído: \(set.key)")
    }

    // MARK: conversão pasta -> .exfat (SPEC §7.4)

    static func safeFileName(_ s: String) -> String {
        let bad = CharacterSet(charactersIn: "/:\\\u{0}")
        let cleaned = s.unicodeScalars.map { bad.contains($0) ? "_" : Character($0) }
        let out = String(cleaned).trimmingCharacters(in: .whitespaces)
        return out.isEmpty ? "jogo" : out
    }

    func convert(_ folder: String, _ ident: Identified) {
        let title = ident.title ?? lastComponent(folder)
        // interrompida de propósito (ex.: ejetar o disco) não é falha: a pasta fica e recomeça depois
        let fail: (String) -> Void = { if !self.isCancelled { self.setProblem(folder, title, $0) } }
        let tools = self.tools
        guard FileManager.default.isExecutableFile(atPath: tools.hdiutil),
              FileManager.default.isExecutableFile(atPath: tools.newfsExfat) else {
            return fail("a conversão para .exfat só funciona no macOS")
        }
        let parent = parentPath(folder)
        let base = Preparer.safeFileName(ident.titleId ?? lastComponent(folder))
        let final = joinPath(parent, base + ".exfat")
        let tmp = final + Identifier.tmpSuffix
        let fm = FileManager.default
        if fm.fileExists(atPath: final) { return fail("já existe \(base).exfat ao lado da pasta") }
        guard isFile(joinPath(folder, "eboot.bin")) else { return fail("pasta sem eboot.bin na raiz") }

        let plan: ExfatPlan
        let files: [(String, String, FileStat)]
        do {
            plan = try ExfatPlan.make(folder: folder, force64K: force64K)
            files = try Library.listFiles(folder)
        } catch {
            return fail("não foi possível listar a pasta: \(error.localizedDescription)")
        }
        if let free = (try? fm.attributesOfFileSystem(forPath: parent))?[.systemFreeSize] as? NSNumber,
           free.int64Value < plan.sizeBytes {
            return fail("espaço insuficiente: precisa de \(plan.sizeMiB) MiB")
        }
        try? fm.removeItem(atPath: tmp)
        var device: String?
        let mountPoint = joinPath(NSTemporaryDirectory(), "ponte-mnt-\(UUID().uuidString)")
        var mounted = false

        func cleanup() {
            if mounted {
                if run(tools.diskutil, ["unmount", mountPoint]).0 != 0 { _ = run(tools.umount, ["-f", mountPoint]) }
            }
            if let device, run(tools.hdiutil, ["detach", device]).0 != 0 {
                _ = run(tools.hdiutil, ["detach", "-force", device])
            }
            try? fm.removeItem(atPath: mountPoint)
        }

        // com um console copiando, nem começa: espera ele terminar
        guard waitWhileCopying() else { return }
        update(step: "criando a imagem (\(plan.sizeMiB) MiB, cluster \(plan.cluster / 1024) KiB)")
        var r = run(tools.mkfile, ["-n", "\(plan.sizeMiB)m", tmp])
        guard r.0 == 0 else { cleanup(); try? fm.removeItem(atPath: tmp); return fail("mkfile falhou: \(r.1)") }
        r = run(tools.hdiutil, ["attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nomount", "-nobrowse", tmp])
        guard r.0 == 0, let dev = RX(#"(/dev/disk\d+)"#).search(r.1)?[1] else {
            cleanup(); try? fm.removeItem(atPath: tmp); return fail("hdiutil attach falhou: \(r.1)")
        }
        device = dev
        r = run(tools.newfsExfat, ["-b", String(plan.cluster), "-v", Preparer.volumeLabel(base), dev])
        guard r.0 == 0 else { cleanup(); try? fm.removeItem(atPath: tmp); return fail("newfs_exfat falhou: \(r.1)") }
        try? fm.createDirectory(atPath: mountPoint, withIntermediateDirectories: true)
        r = run(tools.diskutil, ["mount", "-mountPoint", mountPoint, dev])
        if r.0 != 0 { r = run(tools.mount, ["-t", "exfat", dev, mountPoint]) }
        guard r.0 == 0 else { cleanup(); try? fm.removeItem(atPath: tmp); return fail("não foi possível montar a imagem: \(r.1)") }
        mounted = true

        // cópia arquivo a arquivo, sem atributos estendidos (o exFAT os guardaria como "._*")
        let total = max(1, files.reduce(Int64(0)) { $0 + $1.2.size })
        var copied: Int64 = 0
        update(done: 0, total: total)
        for (rel, abs, _) in files {
            if isCancelled { cleanup(); try? fm.removeItem(atPath: tmp); return }
            update(progress: Double(copied) / Double(total), step: "copiando \(rel)", done: copied)
            let dest = joinPath(mountPoint, rel)
            do {
                try fm.createDirectory(atPath: parentPath(dest), withIntermediateDirectories: true)
                copied += try Preparer.copyFile(abs, to: dest) { n in
                    self.update(progress: Double(copied + n) / Double(total), done: copied + n)
                    // cede o SSD a um console copiando; continua do mesmo ponto depois
                    return self.waitWhileCopying()
                }
            } catch {
                cleanup(); try? fm.removeItem(atPath: tmp)
                if isCancelled { return }  // interrompida (ex.: ejetar o disco): recomeça depois, sem virar falha
                return fail("falha ao copiar \(rel): \(error.localizedDescription)")
            }
        }
        update(progress: 1, step: "finalizando")
        mounted = run(tools.diskutil, ["unmount", mountPoint]).0 != 0 && run(tools.umount, [mountPoint]).0 != 0
        if mounted { cleanup(); try? fm.removeItem(atPath: tmp); return fail("não foi possível desmontar a imagem") }
        cleanup()
        device = nil
        do {
            try fm.moveItem(atPath: tmp, toPath: final)
        } catch {
            try? fm.removeItem(atPath: tmp)
            return fail("não foi possível renomear a imagem: \(error.localizedDescription)")
        }
        var side = ident
        side.kind = "exfat"
        side.warnings = []
        try? Identifier.writeSidecar(side, for: final)
        let icon = joinPath(folder, "sce_sys/icon0.png")
        if isFile(icon) { try? fm.copyItem(atPath: icon, toPath: Identifier.sidecarIconPath(final)) }
        disposeOriginals([folder])
        log("[preparo] convertido: \(title) -> \(base).exfat")
    }

    /// Rótulo do volume: só letras e números, até 15 caracteres.
    static func volumeLabel(_ s: String) -> String {
        let l = String(s.uppercased().filter { $0.isASCII && ($0.isLetter || $0.isNumber) }.prefix(15))
        return l.isEmpty ? "PONTE" : l
    }

    /// Copia em pedaços de 8 MiB. `progress(bytes copiados)` devolve false para cancelar.
    static func copyFile(_ src: String, to dst: String, progress: (Int64) -> Bool) throws -> Int64 {
        guard let input = FileHandle(forReadingAtPath: src) else { throw CocoaError(.fileReadNoPermission) }
        defer { try? input.close() }
        guard FileManager.default.createFile(atPath: dst, contents: nil),
              let output = FileHandle(forWritingAtPath: dst) else { throw CocoaError(.fileWriteNoPermission) }
        defer { try? output.close() }
        var done: Int64 = 0
        while true {
            var chunk = Data()
            var readError: Error?
            autoreleasepoolCompat {
                do { chunk = try input.read(upToCount: 8 << 20) ?? Data() } catch { readError = error }
            }
            if let readError { throw readError }
            if chunk.isEmpty { break }
            try output.write(contentsOf: chunk)
            done += Int64(chunk.count)
            if !progress(done) { throw CocoaError(.userCancelled) }
        }
        return done
    }

    // MARK: utilidades

    /// Move os originais para `_originais/` na raiz da biblioteca, ou apaga.
    func disposeOriginals(_ paths: [String]) {
        let fm = FileManager.default
        if keepOriginals {
            let dir = joinPath(library.root, "_originais")
            try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
            for p in paths {
                do {
                    try fm.moveItem(atPath: p, toPath: Preparer.uniquePath(joinPath(dir, lastComponent(p))))
                } catch {
                    log("[preparo] não foi possível mover \(lastComponent(p)) para _originais: \(error.localizedDescription)")
                }
            }
        } else {
            for p in paths { try? fm.removeItem(atPath: p) }
        }
    }

    /// "x" se não existir; senão "x (2)", "x (3)"… (antes da extensão, para arquivos).
    static func uniquePath(_ path: String) -> String {
        let fm = FileManager.default
        guard fm.fileExists(atPath: path) else { return path }
        let dir = parentPath(path)
        let name = lastComponent(path)
        let ext = (name as NSString).pathExtension
        let stem = ext.isEmpty ? name : String(name.dropLast(ext.count + 1))
        var n = 2
        while true {
            let candidate = joinPath(dir, ext.isEmpty ? "\(stem) (\(n))" : "\(stem) (\(n)).\(ext)")
            if !fm.fileExists(atPath: candidate) { return candidate }
            n += 1
        }
    }

    /// Roda um programa e devolve (código de saída, saída padrão + erro). `onOutput` recebe a saída
    /// conforme chega; `onTick` é chamado a cada 0,25 s enquanto o programa roda. Se um console
    /// começar a copiar, o programa é suspenso (SIGSTOP) e retomado quando a cópia acaba.
    @discardableResult
    func run(_ tool: String, _ args: [String], onOutput: ((String) -> Void)? = nil,
             onTick: (() -> Void)? = nil) -> (Int32, String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        p.standardInput = FileHandle.nullDevice
        #if os(macOS)
        // prioridade baixa para o disco e a CPU: quem manda é o envio para os consoles
        p.qualityOfService = .utility
        #endif
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        do {
            try p.run()
        } catch {
            return (-1, "\(tool): \(error.localizedDescription)")
        }
        locked {
            runningProcess = p
            // cancelado enquanto o processo nascia: `cancel()` não o viu, então encerra aqui
            if cancelled { p.terminate() }
        }
        // leitura numa thread à parte: o pipe não pode encher, senão o programa trava
        let collected = OutputBuffer()
        let reader = Thread {
            let h = pipe.fileHandleForReading
            while true {
                let d = h.availableData
                if d.isEmpty { break }
                let text = String(decoding: d, as: UTF8.self)
                collected.append(text)
                onOutput?(text)
            }
            collected.finish()
        }
        reader.name = "ponte-preparo-saida"
        reader.qualityOfService = .utility
        reader.start()
        var suspended = false
        var first = true
        while p.isRunning {
            // a primeira checagem é na hora: um console que já estava copiando não espera 0,25 s
            if !first { usleep(250_000) }
            first = false
            if isCancelled {
                if suspended { _ = p.resume() }
                p.terminate()
                break
            }
            let yield = mustYield
            if yield && !suspended {
                suspended = p.suspend()
                if suspended { update(paused: true) }
            } else if !yield && suspended {
                _ = p.resume()
                suspended = false
                update(paused: false)
            }
            onTick?()
        }
        p.waitUntilExit()
        collected.waitFinished()
        locked { runningProcess = nil }
        return (p.terminationStatus, collected.text.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    /// Soma dos tamanhos dos arquivos dentro de uma pasta.
    static func directorySize(_ path: String) -> Int64 {
        guard let e = FileManager.default.enumerator(atPath: path) else { return 0 }
        var total: Int64 = 0
        while let rel = e.nextObject() as? String {
            if let st = fileStat(joinPath(path, rel)), st.isRegular { total += st.size }
        }
        return total
    }
}

/// Saída de um programa, juntada por uma thread e lida por outra. Guarda só o fim (o percentual
/// do 7-Zip repete muito).
final class OutputBuffer: @unchecked Sendable {
    private let cond = NSCondition()
    private var buf = ""
    private var done = false
    static let keep = 64 * 1024

    func append(_ s: String) {
        cond.lock()
        buf += s
        if buf.utf8.count > OutputBuffer.keep * 2 { buf = String(buf.suffix(OutputBuffer.keep)) }
        cond.unlock()
    }

    func finish() {
        cond.lock()
        done = true
        cond.broadcast()
        cond.unlock()
    }

    func waitFinished() {
        cond.lock()
        while !done { cond.wait() }
        cond.unlock()
    }

    var text: String {
        cond.lock()
        defer { cond.unlock() }
        return buf
    }
}
