import Foundation
import PonteCore

/// Uma cópia em andamento, vista do Mac: o que um console está baixando agora.
///
/// O Mac mede pelo que ele mesmo serve, sem nada novo no protocolo: para cada arquivo, até onde
/// já entregou (início do `Range` + bytes enviados, então a retomada conta o que o console já
/// tinha); a velocidade é a dos últimos segundos.
public struct TransferSnapshot: Equatable, Sendable, Identifiable {
    public let id: String
    public let itemId: String
    public let title: String
    public let platform: String
    public let kind: String
    /// Nome do console (ou o IP, sem pareamento).
    public let client: String
    public let startedAt: Date
    public let lastActivity: Date
    public let totalBytes: Int64
    /// Bytes que o console já tem, somando todos os arquivos.
    public let doneBytes: Int64
    /// Bytes enviados nesta cópia (sem contar o que veio de uma cópia anterior retomada).
    public let sentBytes: Int64
    public let fileCount: Int
    /// Arquivo sendo enviado agora (ou o último), relativo à raiz do item.
    public let currentFile: String?
    public let currentFileIndex: Int?
    public let bytesPerSecond: Double
    /// Há um envio aberto neste instante.
    public let streaming: Bool
    /// O console já mandou o resultado, mas o arquivo ainda está sendo baixado (no PS5, o
    /// instalador do sistema baixa o `.pkg` depois de o payload dizer `installed`).
    public let reportedResult: String?

    public var fraction: Double {
        totalBytes > 0 ? min(1, Double(doneBytes) / Double(totalBytes)) : 0
    }

    /// Segundos até terminar, na velocidade atual.
    public var secondsLeft: Double? {
        guard bytesPerSecond >= 1, doneBytes < totalBytes else { return nil }
        return Double(totalBytes - doneBytes) / bytesPerSecond
    }
}

/// Uma cópia terminada, no histórico (guardado entre execuções).
public struct CopyRecord: Codable, Equatable, Sendable, Identifiable {
    public var id: String
    public var itemId: String
    public var title: String
    public var platform: String
    public var kind: String
    public var content: String?
    public var version: String?
    public var client: String
    /// `installed` | `copied` | `failed` | `cancelled` (do console) ou `interrupted` (o console
    /// parou de baixar e não mandou resultado: queda de rede, console desligado, SSD ejetado…).
    public var result: String
    public var storage: String?
    public var detail: String?
    /// Primeiro e último byte enviados pelo Mac; nil quando o console só mandou o resultado.
    public var startedAt: Date?
    public var lastActivity: Date?
    public var finishedAt: Date
    /// Duração informada pelo console.
    public var durationS: Int?
    public var totalBytes: Int64
    public var sentBytes: Int64

    /// Tempo de envio medido pelo Mac.
    public var transferSeconds: Double? {
        guard let s = startedAt, let e = lastActivity else { return nil }
        let d = e.timeIntervalSince(s)
        return d > 0 ? d : nil
    }

    public var averageBytesPerSecond: Double? {
        guard sentBytes > 0, let d = transferSeconds ?? durationS.map(Double.init), d > 0 else { return nil }
        return Double(sentBytes) / d
    }
}

/// Um envio de arquivo (uma resposta HTTP). Criado na rota e ligado pelo servidor HTTP quando o
/// corpo começa a sair.
final class TransferStream: @unchecked Sendable {
    private let tracker: TransferTracker
    private let key: String
    private let item: Item
    private let client: String
    private let index: Int
    private let offset: Int64
    private var sent: Int64 = 0
    private var started = false

    init(tracker: TransferTracker, key: String, item: Item, client: String, index: Int, offset: Int64) {
        self.tracker = tracker
        self.key = key
        self.item = item
        self.client = client
        self.index = index
        self.offset = offset
    }

    var hooks: HTTPServer.TransferHooks {
        HTTPServer.TransferHooks(start: { self.start() }, sent: { self.add($0) }, end: { self.end() })
    }

    func start() {
        started = true
        tracker.streamStarted(key: key, item: item, client: client, index: index, offset: offset)
    }

    func add(_ n: Int64) {
        sent += n
        tracker.streamSent(key: key, index: index, reached: offset + sent, delta: n)
    }

    func end() {
        guard started else { return }
        started = false
        tracker.streamEnded(key: key)
    }
}

/// Cópias em andamento e histórico.
final class TransferTracker: @unchecked Sendable {
    /// Sem envio e sem resultado por este tempo → vai para o histórico como `interrupted`.
    var idleToInterrupted: TimeInterval = 120
    /// Resultado já recebido, mas ainda baixando (`.pkg` do PS5): termina depois deste tempo parado.
    var reportedGrace: TimeInterval = 30
    /// Por quanto tempo um `installed` de `.pkg` espera o instalador começar a baixar.
    var pendingInstallWindow: TimeInterval = 30 * 60
    static let maxHistory = 500
    static let speedWindow: TimeInterval = 5

    private struct Report {
        let result: String
        let storage: String?
        let detail: String?
        let durationS: Int?
    }

    private struct Live {
        let itemId: String
        let title: String
        let platform: String
        let kind: String
        let content: String
        let version: String?
        let client: String
        let paths: [String]
        let sizes: [Int64]
        let startedAt: Date
        var lastActivity: Date
        var reached: [Int: Int64] = [:]
        var sent: Int64 = 0
        var streams = 0
        var currentIndex: Int?
        var samples: [(Date, Int64)] = []
        var report: Report?
        /// Registro do histórico que esta cópia completa (instalação de `.pkg` já informada).
        var recordId: String?

        var done: Int64 {
            sizes.indices.reduce(Int64(0)) { $0 + min(reached[$1] ?? 0, sizes[$1]) }
        }
    }

    private let lock = NSLock()
    private let historyFile: String?
    private let now: () -> Date
    private var live: [String: Live] = [:]
    private var history: [CopyRecord] = []
    private var pendingInstall: [String: (recordId: String, at: Date)] = [:]
    var log: (String) -> Void = { _ in }

    init(historyFile: String?, now: @escaping () -> Date = Date.init) {
        self.historyFile = historyFile
        self.now = now
        if let historyFile, let data = FileManager.default.contents(atPath: historyFile) {
            let dec = JSONDecoder()
            dec.dateDecodingStrategy = .secondsSince1970
            history = (try? dec.decode([CopyRecord].self, from: data)) ?? []
        }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    static func liveKey(itemId: String, client: String) -> String { itemId + "|" + client }

    // MARK: eventos dos envios

    func streamStarted(key: String, item: Item, client: String, index: Int, offset: Int64) {
        locked {
            let t = now()
            sweepLocked(t)
            // cópia anterior já terminada (com resultado) e o console começou outra: fecha a anterior
            if let old = live[key], old.report != nil, old.streams == 0, old.recordId == nil {
                finalizeLocked(key, at: t)
            }
            if var l = live[key] {
                l.streams += 1
                l.currentIndex = index
                l.reached[index] = max(l.reached[index] ?? 0, offset)
                l.lastActivity = t
                live[key] = l
                return
            }
            var l = Live(itemId: item.id, title: item.title, platform: item.platform, kind: item.ident.kind,
                         content: item.ident.content, version: item.ident.version, client: client,
                         paths: item.files.map(\.path), sizes: item.files.map(\.size), startedAt: t, lastActivity: t)
            l.streams = 1
            l.currentIndex = index
            l.reached[index] = offset
            // o console já disse "installed" para este .pkg: é o instalador baixando
            let ckey = key
            if let p = pendingInstall[ckey], t.timeIntervalSince(p.at) <= pendingInstallWindow,
               let rec = history.first(where: { $0.id == p.recordId }) {
                l.recordId = rec.id
                l.report = Report(result: rec.result, storage: rec.storage, detail: rec.detail, durationS: rec.durationS)
            }
            live[key] = l
        }
    }

    func streamSent(key: String, index: Int, reached: Int64, delta: Int64) {
        locked {
            guard var l = live[key] else { return }
            let t = now()
            l.reached[index] = max(l.reached[index] ?? 0, reached)
            l.sent += delta
            l.lastActivity = t
            l.samples.append((t, l.sent))
            if let first = l.samples.first, t.timeIntervalSince(first.0) > TransferTracker.speedWindow * 2 {
                l.samples.removeAll { t.timeIntervalSince($0.0) > TransferTracker.speedWindow }
            }
            live[key] = l
        }
    }

    func streamEnded(key: String) {
        locked {
            guard var l = live[key] else { return }
            let t = now()
            l.streams = max(0, l.streams - 1)
            l.lastActivity = t
            live[key] = l
            // o resultado chegou um instante antes de o envio terminar aqui: fecha agora. Um .pkg
            // espera um pouco, porque o instalador do PS5 baixa em várias partes
            if l.streams == 0, l.report != nil, !l.kind.hasPrefix("pkg") {
                finalizeLocked(key, at: t)
            }
        }
    }

    // MARK: resultado do console (SPEC §5.8)

    func report(item: Item, client: String, key: String, result: String, storage: String?, detail: String?,
                durationS: Int?) {
        locked {
            let t = now()
            let rep = Report(result: result, storage: storage, detail: detail, durationS: durationS)
            if var l = live[key] {
                l.report = rep
                live[key] = l
                if l.streams == 0 { finalizeLocked(key, at: t) }
                return
            }
            // nada baixado agora (falha antes de começar, ou .pkg que o instalador vai baixar depois)
            let rec = CopyRecord(id: TransferTracker.newId(), itemId: item.id, title: item.title, platform: item.platform,
                                 kind: item.ident.kind, content: item.ident.content, version: item.ident.version,
                                 client: client, result: result, storage: storage, detail: detail,
                                 startedAt: nil, lastActivity: nil, finishedAt: t, durationS: durationS,
                                 totalBytes: item.size, sentBytes: 0)
            appendLocked(rec)
            if result == "installed", item.ident.kind.hasPrefix("pkg") {
                pendingInstall[key] = (rec.id, t)
            }
        }
    }

    // MARK: consulta

    /// Cópias em andamento, mais recentes primeiro.
    func snapshot() -> [TransferSnapshot] {
        locked {
            let t = now()
            sweepLocked(t)
            return live.map { key, l in
                var speed = 0.0
                if l.streams > 0, let first = l.samples.first(where: { t.timeIntervalSince($0.0) <= TransferTracker.speedWindow }),
                   let last = l.samples.last {
                    let span = max(t.timeIntervalSince(first.0), 1)
                    speed = Double(last.1 - first.1) / span
                }
                let idx = l.currentIndex
                return TransferSnapshot(id: key, itemId: l.itemId, title: l.title, platform: l.platform, kind: l.kind,
                                        client: l.client, startedAt: l.startedAt, lastActivity: l.lastActivity,
                                        totalBytes: l.sizes.reduce(0, +), doneBytes: l.done, sentBytes: l.sent,
                                        fileCount: l.sizes.count, currentFile: idx.flatMap { l.paths.indices.contains($0) ? l.paths[$0] : nil },
                                        currentFileIndex: idx, bytesPerSecond: speed, streaming: l.streams > 0,
                                        reportedResult: l.report?.result)
            }
            .sorted { $0.startedAt > $1.startedAt }
        }
    }

    /// Itens com envio aberto, ou com atividade nos últimos `grace` segundos e ainda sem
    /// resultado (cobre a pausa entre um arquivo e o próximo).
    func activeItemIds(grace: TimeInterval) -> Set<String> {
        locked {
            let t = now()
            var ids = Set<String>()
            for l in live.values where l.streams > 0 || (l.report == nil && t.timeIntervalSince(l.lastActivity) <= grace) {
                ids.insert(l.itemId)
            }
            return ids
        }
    }

    /// Histórico, mais recente primeiro.
    func records() -> [CopyRecord] {
        locked {
            sweepLocked(now())
            return history.reversed()
        }
    }

    func clearHistory() {
        locked {
            history = []
            pendingInstall = [:]
            saveLocked()
        }
    }

    // MARK: interno

    private func sweepLocked(_ t: Date) {
        for (key, l) in live where l.streams == 0 {
            let idle = t.timeIntervalSince(l.lastActivity)
            if l.report != nil ? idle >= reportedGrace : idle >= idleToInterrupted {
                finalizeLocked(key, at: t)
            }
        }
        pendingInstall = pendingInstall.filter { t.timeIntervalSince($0.value.at) <= pendingInstallWindow }
    }

    private func finalizeLocked(_ key: String, at t: Date) {
        guard let l = live.removeValue(forKey: key) else { return }
        if let rid = l.recordId, let i = history.firstIndex(where: { $0.id == rid }) {
            // instalação de .pkg: completa o registro que o "installed" criou
            var rec = history[i]
            rec.startedAt = rec.startedAt ?? l.startedAt
            rec.lastActivity = l.lastActivity
            rec.sentBytes += l.sent
            history[i] = rec
            pendingInstall[key] = (rid, t)  // se o instalador voltar a baixar, continua no mesmo registro
            saveLocked()
            return
        }
        let rep = l.report
        let rec = CopyRecord(id: TransferTracker.newId(), itemId: l.itemId, title: l.title, platform: l.platform,
                             kind: l.kind, content: l.content, version: l.version, client: l.client,
                             result: rep?.result ?? "interrupted", storage: rep?.storage, detail: rep?.detail,
                             startedAt: l.startedAt, lastActivity: l.lastActivity, finishedAt: t,
                             durationS: rep?.durationS, totalBytes: l.sizes.reduce(0, +), sentBytes: l.sent)
        if rep == nil { log("[cópia] \(l.title): interrompida (\(l.client) parou de baixar sem mandar o resultado)") }
        appendLocked(rec)
        if rep?.result == "installed", l.kind.hasPrefix("pkg") { pendingInstall[key] = (rec.id, t) }
    }

    private func appendLocked(_ rec: CopyRecord) {
        history.append(rec)
        if history.count > TransferTracker.maxHistory { history.removeFirst(history.count - TransferTracker.maxHistory) }
        saveLocked()
    }

    private func saveLocked() {
        guard let historyFile else { return }
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .secondsSince1970
        enc.outputFormatting = [.sortedKeys]
        if let data = try? enc.encode(history) {
            try? data.write(to: URL(fileURLWithPath: historyFile), options: .atomic)
        }
    }

    static func newId() -> String {
        var g = SystemRandomNumberGenerator()
        return (0..<8).map { _ in String(format: "%02x", UInt8.random(in: 0...255, using: &g)) }.joined()
    }
}
