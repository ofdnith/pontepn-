import Foundation
import PonteCore

public let ponteProtocolVersion = 1
public let ponteServerVersion = "1.0.2"

/// Erro de API no formato da SPEC §6.
public struct APIError: Error {
    public let status: Int
    public let code: String
    public let message: String

    public init(_ status: Int, _ code: String, _ message: String) {
        self.status = status
        self.code = code
        self.message = message
    }
}

/// Console pareado (gravado em tokens.json, no mesmo formato da referência).
public struct PairedClient: Equatable, Sendable {
    public var platform: String
    public var name: String
    public var version: String
    public var firmware: String?
    public var pairedAt: Int

    var json: [String: Any] {
        ["platform": platform, "name": name, "version": version, "firmware": firmware as Any? ?? NSNull(), "paired_at": pairedAt]
    }

    init(platform: String, name: String, version: String, firmware: String?, pairedAt: Int) {
        self.platform = platform
        self.name = name
        self.version = version
        self.firmware = firmware
        self.pairedAt = pairedAt
    }

    init?(json: Any) {
        guard let d = json as? [String: Any], let p = d["platform"] as? String else { return nil }
        platform = p
        name = d["name"] as? String ?? ""
        version = d["version"] as? String ?? ""
        firmware = d["firmware"] as? String
        pairedAt = (d["paired_at"] as? NSNumber)?.intValue ?? 0
    }
}

/// Cópia em andamento: um console baixando arquivos de um item.
public struct ActiveTransfer: Equatable, Sendable {
    public let itemId: String
    public let title: String
}

public struct PonteConfig {
    public var library: String
    public var profilesDir: String
    public var stateDir: String
    public var name: String
    public var host = "0.0.0.0"
    public var port = 47101
    public var discoveryHost = "0.0.0.0"
    /// nil = sem descoberta UDP.
    public var discoveryPort: Int? = 47100
    public var pairing = true
    public var rescanInterval = 5.0
    public var stableSeconds = 10.0
    /// Converter pastas de PS5 em .exfat antes de entrarem no catálogo (SPEC §7.4).
    public var convertFolders = false
    /// Extrair compactados (SPEC §1.3).
    public var extractArchives = false
    /// Depois de extrair/converter: mover os originais para `_originais/` (true) ou apagar (false).
    public var keepOriginals = true
    public var force64KCluster = false
    /// Calcular sha256 e informar aos consoles (SPEC §7.5). O app deixa desligado por padrão.
    public var computeHashes = true

    public init(library: String, profilesDir: String, stateDir: String, name: String) {
        self.library = library
        self.profilesDir = profilesDir
        self.stateDir = stateDir
        self.name = name
    }
}

/// Servidor do protocolo Ponte (SPEC §2 a §6): o mesmo comportamento do servidor de referência em
/// Python (`reference/ponte_ref/server.py`), validado por `tools/conformance.py`.
public final class PonteServer: @unchecked Sendable {
    static let api = "/api/v1"
    static let pairMaxFails = 5
    static let pairWindow = 60.0
    static let platforms: Set<String> = ["ps5", "switch"]
    static let routeItem = RX(#"^/api/v1/items/([0-9a-f]{16})$"#)
    // o nome no fim (opcional) é só para quem baixa enxergar a extensão, ex.: o instalador do PS5
    static let routeFile = RX(#"^/api/v1/items/([0-9a-f]{16})/files/(\d+)(?:/[^/]+)?$"#)
    static let routeIcon = RX(#"^/api/v1/items/([0-9a-f]{16})/icon$"#)
    static let routeReport = RX(#"^/api/v1/items/([0-9a-f]{16})/report$"#)
    static let rangeRE = RX(#"^bytes=(\d*)-(\d*)$"#)

    public let config: PonteConfig
    public let library: Library
    public private(set) var profiles: [Profile]
    public let serverId: String
    public var log: (String) -> Void

    private let lock = NSLock()
    private var tokens: [String: PairedClient]
    private var _code: String
    private var fails: [String: [Double]] = [:]
    let transfers: TransferTracker
    private var _name: String
    private var _pairing: Bool

    private var http: HTTPServer?
    private var discovery: DiscoveryResponder?
    public let preparer: Preparer
    private let stopFlag = StopFlag()
    private let backgroundThreads = ThreadCount()

    public var port: Int { http?.port ?? config.port }
    public var discoveryPort: Int? { discovery?.port }

    public init(config: PonteConfig, log: @escaping (String) -> Void = { print($0) }) throws {
        self.config = config
        self.log = log
        try FileManager.default.createDirectory(atPath: config.stateDir, withIntermediateDirectories: true)
        library = Library(root: config.library, stableSeconds: config.stableSeconds,
                          hashCacheFile: joinPath(config.stateDir, "hashes.json"))
        library.holdFolders = config.convertFolders
        library.hashesEnabled = config.computeHashes
        profiles = Profiles.load(directory: config.profilesDir)
        _name = config.name
        _pairing = config.pairing
        _code = PonteServer.newCode()

        let stateDir = config.stateDir
        func load(_ name: String) -> Any? {
            FileManager.default.contents(atPath: joinPath(stateDir, name)).flatMap(JSON.object)
        }
        if let d = load("server.json") as? [String: Any], let sid = d["id"] as? String,
           RX("^[0-9a-f]{16}$").matches(sid) {
            serverId = sid
        } else {
            serverId = PonteServer.randomHex(8)
            try? JSON.data(["id": serverId], pretty: true).write(to: URL(fileURLWithPath: joinPath(stateDir, "server.json")), options: .atomic)
        }
        var t: [String: PairedClient] = [:]
        for (k, v) in load("tokens.json") as? [String: Any] ?? [:] {
            if let c = PairedClient(json: v) { t[k] = c }
        }
        tokens = t
        transfers = TransferTracker(historyFile: joinPath(config.stateDir, "history.json"))
        transfers.log = log
        preparer = Preparer(library: library, log: log)
        preparer.convertFolders = config.convertFolders
        preparer.extractArchives = config.extractArchives
        preparer.keepOriginals = config.keepOriginals
        preparer.force64K = config.force64KCluster
        // com um console copiando, o SSD e a rede são dele: extração e conversão esperam
        preparer.shouldYield = { [weak self] in self?.isCopying ?? false }
    }

    /// Algum console baixou um arquivo nos últimos segundos.
    public var isCopying: Bool { !transfers.activeItemIds(grace: 5).isEmpty }

    /// Cálculo de hashes em andamento.
    public var hashing: Library.HashProgress? { library.hashing }

    // MARK: prioridade durante as cópias

    private var openStreams = 0
    #if os(macOS)
    private var copyActivity: NSObjectProtocol?
    #endif

    /// Um envio de arquivo começou: enquanto houver algum, o Mac não entra em repouso sozinho
    /// (repouso no meio derrubaria a cópia) e o app fica com prioridade de interação.
    private func streamBegan() {
        lock.lock()
        openStreams += 1
        #if os(macOS)
        if openStreams == 1 && copyActivity == nil {
            copyActivity = ProcessInfo.processInfo.beginActivity(
                options: [.userInitiated, .latencyCritical],
                reason: "Ponte: um console está copiando")
        }
        #endif
        lock.unlock()
    }

    private func streamEnded() {
        lock.lock()
        openStreams = max(0, openStreams - 1)
        #if os(macOS)
        if openStreams == 0, let a = copyActivity {
            ProcessInfo.processInfo.endActivity(a)
            copyActivity = nil
        }
        #endif
        lock.unlock()
    }

    // MARK: ciclo de vida

    #if os(macOS)
    /// Sem isto, com a janela do app escondida o macOS põe o app em "App Nap" e o envio pela rede
    /// cai para quase nada. Não impede o Mac de dormir.
    private var activity: NSObjectProtocol?
    #endif

    public func start() throws {
        #if os(macOS)
        if activity == nil {
            activity = ProcessInfo.processInfo.beginActivity(
                options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
                reason: "Ponte: servindo a biblioteca para os consoles")
        }
        #endif
        library.scan()
        let http = try HTTPServer(host: config.host, port: config.port) { [weak self] req in
            guard let self else { return HTTPServer.jsonError(500, "internal", "servidor parado") }
            return self.handle(req)
        }
        self.http = http
        if let dp = config.discoveryPort {
            do {
                discovery = try DiscoveryResponder(host: config.discoveryHost, port: dp) { [weak self] in
                    guard let self else { return Data() }
                    return JSON.data(["ponte": ponteProtocolVersion, "name": self.name, "port": self.port, "id": self.serverId])
                }
            } catch {
                http.stop()
                throw error
            }
        }
        http.start()
        discovery?.start()
        startBackground()
    }

    public func stop() {
        #if os(macOS)
        if let activity { ProcessInfo.processInfo.endActivity(activity) }
        activity = nil
        #endif
        stopFlag.set()
        preparer.cancel()
        http?.stop()
        discovery?.stop()
    }

    /// Para tudo e espera soltar os arquivos da biblioteca: conexões fechadas, preparo interrompido
    /// (a imagem de conversão é desanexada) e varredura e hashes parados. Usado antes de ejetar o
    /// disco. false = algo não terminou dentro do prazo.
    @discardableResult
    public func stopAndWait(timeout: TimeInterval = 15) -> Bool {
        stop()
        let deadline = Date(timeIntervalSinceNow: timeout)
        func left() -> TimeInterval { max(0, deadline.timeIntervalSinceNow) }
        let httpClosed = http?.waitUntilClosed(timeout: left()) ?? true
        let prepIdle = preparer.waitUntilIdle(timeout: left())
        let threadsDone = backgroundThreads.waitUntilZero(timeout: left())
        return httpClosed && prepIdle && threadsDone
    }

    /// Itens sendo copiados por algum console agora, ou nos últimos `grace` segundos.
    public func activeTransfers(grace: TimeInterval = 30) -> [ActiveTransfer] {
        transfers.activeItemIds(grace: grace).sorted().map { id in
            ActiveTransfer(itemId: id, title: library.get(id)?.title ?? id)
        }
    }

    /// Cópias em andamento com progresso, velocidade e arquivo atual.
    public var currentTransfers: [TransferSnapshot] { transfers.snapshot() }

    /// Cópias terminadas, mais recentes primeiro (guardado em `history.json` na pasta de estado).
    public var history: [CopyRecord] { transfers.records() }

    public func clearHistory() { transfers.clearHistory() }

    private func startBackground() {
        let stop = stopFlag
        let count = backgroundThreads
        count.enter()
        count.enter()
        let scanner = Thread { [weak self] in
            defer { count.leave() }
            while !stop.isSet {
                stop.wait(self?.config.rescanInterval ?? 5)
                guard let self, !stop.isSet else { return }
                self.library.scan()
                self.preparer.tick(shouldStop: { stop.isSet })
            }
        }
        scanner.name = "ponte-scan"
        // prioridade baixa também para o disco: no macOS a leitura fica "estrangulada" e cede a vez
        // ao Finder e a quem mais estiver usando o SSD
        scanner.qualityOfService = .utility
        scanner.start()
        let hasher = Thread { [weak self] in
            defer { count.leave() }
            while !stop.isSet {
                guard let self else { return }
                // com um console copiando, o SSD é dele: o cálculo de hashes espera
                let copying = { self.isCopying }
                if copying() {
                    stop.wait(2)
                    continue
                }
                let done = self.library.hashPending(shouldStop: { stop.isSet || copying() })
                if done == 0 { stop.wait(1) }
            }
        }
        hasher.name = "ponte-hash"
        hasher.qualityOfService = .background
        hasher.start()
    }

    /// Força uma varredura agora (ex.: botão no app).
    public func rescan() {
        library.scan()
    }

    public func reloadProfiles() {
        let p = Profiles.load(directory: config.profilesDir)
        lock.lock()
        profiles = p
        lock.unlock()
    }

    // MARK: estado para o app

    public var code: String { locked { _code } }
    public var pairedClients: [(token: String, client: PairedClient)] {
        locked { tokens.map { ($0.key, $0.value) }.sorted { $0.client.pairedAt < $1.client.pairedAt } }
    }

    public var name: String {
        get { locked { _name } }
        set { locked { _name = newValue } }
    }

    public var pairingRequired: Bool {
        get { locked { _pairing } }
        set { locked { _pairing = newValue } }
    }

    /// Gera outro código de pareamento (o anterior deixa de valer).
    public func rotateCode() {
        let c = locked { () -> String in
            _code = PonteServer.newCode()
            return _code
        }
        log("[pair] Novo código: \(c)")
    }

    public func revoke(token: String) {
        locked {
            tokens[token] = nil
            saveTokens()
        }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    // MARK: pareamento (SPEC §4)

    static func newCode() -> String {
        let n = Int.random(in: 0..<1_000_000)
        let s = String(n)
        return String(repeating: "0", count: 6 - s.count) + s
    }

    static func randomHex(_ bytes: Int) -> String {
        var g = SystemRandomNumberGenerator()
        return hexString((0..<bytes).map { _ in UInt8.random(in: 0...255, using: &g) })
    }

    /// Comparação em tempo constante.
    static func sameSecret(_ a: String, _ b: String) -> Bool {
        let x = Array(a.utf8), y = Array(b.utf8)
        guard x.count == y.count else { return false }
        var diff: UInt8 = 0
        for i in 0..<x.count { diff |= x[i] ^ y[i] }
        return diff == 0
    }

    private func saveTokens() {
        let obj = tokens.mapValues(\.json)
        try? JSON.data(obj, pretty: true).write(to: URL(fileURLWithPath: joinPath(config.stateDir, "tokens.json")), options: .atomic)
    }

    func pair(ip: String, body: [String: Any]) throws -> [String: Any] {
        let now = Date().timeIntervalSince1970
        let (token, message): (String, String) = try locked {
            var recent = (fails[ip] ?? []).filter { now - $0 < PonteServer.pairWindow }
            fails[ip] = recent
            if recent.count >= PonteServer.pairMaxFails {
                throw APIError(429, "too_many_attempts", "tentativas demais; aguarde 1 minuto")
            }
            guard let code = body["code"] as? String, PonteServer.sameSecret(code, _code) else {
                recent.append(now)
                fails[ip] = recent
                throw APIError(401, "unauthorized", "código de pareamento errado")
            }
            let client = body["client"] as? [String: Any] ?? [:]
            guard let platform = client["platform"] as? String, PonteServer.platforms.contains(platform) else {
                throw APIError(400, "bad_request", "client.platform deve ser ps5 ou switch")
            }
            let token = PonteServer.randomHex(32)
            let info = PairedClient(platform: platform, name: String(String(describingField: client["name"]).prefix(64)),
                                    version: String(String(describingField: client["version"]).prefix(32)),
                                    firmware: client["firmware"] as? String, pairedAt: Int(now))
            tokens[token] = info
            saveTokens()
            fails[ip] = nil
            _code = PonteServer.newCode()
            return (token, "[pair] \(info.name.isEmpty ? ip : info.name) pareado (\(platform)). Novo código: \(_code)")
        }
        log(message)
        return ["client_token": token]
    }

    /// Cliente da requisição: `[:]`-equivalente quando o pareamento está desligado; nil = sem acesso.
    func client(for auth: String?) -> PairedClient? {
        if !pairingRequired { return PairedClient(platform: "", name: "", version: "", firmware: nil, pairedAt: 0) }
        guard let auth, auth.hasPrefix("Bearer ") else { return nil }
        let token = auth.dropFirst(7).trimmingCharacters(in: .whitespaces)
        return locked { tokens[token] }
    }

    func updateFirmware(auth: String?, firmware: String?) {
        guard pairingRequired, let auth, auth.hasPrefix("Bearer "), let firmware else { return }
        let token = auth.dropFirst(7).trimmingCharacters(in: .whitespaces)
        locked {
            if var info = tokens[token], info.firmware != firmware {
                info.firmware = firmware
                tokens[token] = info
                saveTokens()
            }
        }
    }

    // MARK: rotas (SPEC §5)

    func handle(_ req: HTTPServer.Request) -> HTTPServer.Response {
        do {
            var body: [String: Any]?
            if req.method == "POST" { body = try readBody(req) }
            if let v = req.header("X-Ponte-Protocol"), v.trimmingCharacters(in: .whitespaces) != String(ponteProtocolVersion) {
                throw APIError(426, "protocol_mismatch", "servidor fala o protocolo \(ponteProtocolVersion)")
            }
            return try route(req, body: body ?? [:])
        } catch let e as APIError {
            var r = HTTPServer.jsonError(e.status, e.code, e.message)
            if req.bodyTooLarge { r.close = true }
            return r
        } catch {
            return HTTPServer.jsonError(500, "internal", "\(error)")
        }
    }

    func readBody(_ req: HTTPServer.Request) throws -> [String: Any] {
        if req.bodyTooLarge { throw APIError(400, "bad_request", "corpo grande demais") }
        if req.body.isEmpty { return [:] }
        guard let obj = JSON.object(req.body) else { throw APIError(400, "bad_request", "JSON inválido") }
        guard let d = obj as? [String: Any] else { throw APIError(400, "bad_request", "o corpo deve ser um objeto JSON") }
        return d
    }

    static func json(_ status: Int, _ payload: Any) -> HTTPServer.Response {
        HTTPServer.Response(status: status, headers: [("Content-Type", "application/json; charset=utf-8")],
                            body: .data(JSON.data(payload)))
    }

    func item(_ id: String) throws -> Item {
        if let it = library.get(id) { return it }
        if library.pendingIds().contains(id) {
            throw APIError(409, "item_not_ready", "item ainda sendo preparado")
        }
        throw APIError(404, "not_found", "item não encontrado")
    }

    func catalogItems(platform: String, firmware: String?) -> [[String: Any]] {
        let (profile, guessed) = Profiles.select(locked { profiles }, platform: platform, firmware: firmware)
        return library.items(platform: platform).map { it in
            var d = it.summary()
            d["warnings"] = uniqued(it.allWarnings
                + ConsoleState.profileWarnings(kind: it.ident.kind, profile: profile, guessed: guessed))
            return d
        }
    }

    func route(_ req: HTTPServer.Request, body: [String: Any]) throws -> HTTPServer.Response {
        let method = req.method
        let get = method == "GET" || method == "HEAD"
        var path = req.path
        while path.count > 1 && path.hasSuffix("/") { path.removeLast() }
        let query = req.query
        let api = PonteServer.api

        if path == "\(api)/info" && get {
            return PonteServer.json(200, ["protocol": ponteProtocolVersion, "server": "ponte-mac/\(ponteServerVersion)",
                                          "name": name, "id": serverId, "pairing_required": pairingRequired])
        }
        if path == "\(api)/pair" && method == "POST" {
            if !pairingRequired { return PonteServer.json(200, ["client_token": ""]) }
            return PonteServer.json(200, try pair(ip: req.remoteIP, body: body))
        }

        var auth = req.header("Authorization")
        if auth == nil, get, PonteServer.routeFile.matches(path), let token = query["token"] {
            // rotas de arquivo também aceitam ?token= (SPEC §5.5): para quem baixa não manda cabeçalho
            auth = "Bearer " + token
        }
        guard let client = client(for: auth) else {
            throw APIError(401, "unauthorized", "pareamento necessário")
        }
        let clientPlatform = client.platform.isEmpty ? nil : client.platform
        // quem é o console nas cópias: o token (o instalador do PS5 usa o mesmo, via ?token=) ou o IP
        let token = auth.flatMap { $0.hasPrefix("Bearer ") ? $0.dropFirst(7).trimmingCharacters(in: .whitespaces) : nil }
        let clientKey = pairingRequired ? (token ?? req.remoteIP) : req.remoteIP
        let clientLabel = PonteServer.clientLabel(client, ip: req.remoteIP)

        if path == "\(api)/catalog" && get {
            guard let platform = query["platform"] ?? clientPlatform, PonteServer.platforms.contains(platform) else {
                throw APIError(400, "bad_request", "informe platform=ps5 ou platform=switch")
            }
            return PonteServer.json(200, ["generated_at": library.generatedAt,
                                          "items": catalogItems(platform: platform, firmware: client.firmware)])
        }

        if path == "\(api)/catalog/changes" && get {
            guard let since = ISOTime.parse(query["since"]) else {
                throw APIError(400, "bad_request", "data inválida; use AAAA-MM-DDTHH:MM:SSZ")
            }
            let p = query["platform"] ?? clientPlatform
            let items = library.items(platform: p.flatMap { PonteServer.platforms.contains($0) ? $0 : nil })
            let n = items.filter { $0.addedTS > since }.count
            return PonteServer.json(200, ["changed": n > 0, "new_items": n, "generated_at": library.generatedAt])
        }

        if path == "\(api)/console/state" && method == "POST" {
            guard let platform = (body["platform"] as? String) ?? clientPlatform, PonteServer.platforms.contains(platform) else {
                throw APIError(400, "bad_request", "platform deve ser ps5 ou switch")
            }
            if let v = body["installed"], !(v is [Any]) { throw APIError(400, "bad_request", "installed e storages devem ser listas") }
            if let v = body["storages"], !(v is [Any]) { throw APIError(400, "bad_request", "installed e storages devem ser listas") }
            var console = body
            console["platform"] = platform
            updateFirmware(auth: req.header("Authorization"), firmware: body["firmware"] as? String)
            let items = ConsoleState.annotate(library.items(platform: platform), console: console, profiles: locked { profiles })
            return PonteServer.json(200, ["generated_at": library.generatedAt, "items": items])
        }

        if get, let m = PonteServer.routeItem.search(path) {
            return PonteServer.json(200, library.detail(try item(m[1]!)))
        }
        if get, let m = PonteServer.routeFile.search(path) {
            return try sendFile(try item(m[1]!), index: Int(m[2]!) ?? Int.max, req: req,
                                client: clientLabel, clientKey: clientKey)
        }
        if get, let m = PonteServer.routeIcon.search(path) {
            let it = try item(m[1]!)
            guard let data = it.iconBytes(), !data.isEmpty else { throw APIError(404, "not_found", "item sem ícone") }
            let png = data.starts(with: [0x89, 0x50, 0x4E, 0x47])
            return HTTPServer.Response(status: 200, headers: [("Content-Type", png ? "image/png" : "image/jpeg")], body: .data(data))
        }
        if method == "POST", let m = PonteServer.routeReport.search(path) {
            let it = try item(m[1]!)
            guard let result = body["result"] as? String, ["installed", "copied", "failed", "cancelled"].contains(result) else {
                throw APIError(400, "bad_request", "result inválido")
            }
            let detail = body["detail"] as? String
            transfers.report(item: it, client: clientLabel, key: TransferTracker.liveKey(itemId: it.id, client: clientKey),
                             result: result, storage: body["storage"] as? String, detail: detail,
                             durationS: (body["duration_s"] as? NSNumber)?.intValue)
            log("[report] \(it.title): \(result)" + (detail.map { " (\($0))" } ?? ""))
            return HTTPServer.Response(status: 204)
        }
        throw APIError(404, "not_found", "rota não encontrada")
    }

    static func parseRange(_ header: String, size: Int64) throws -> (Int64, Int64) {
        guard let m = rangeRE.search(header.trimmingCharacters(in: .whitespaces)),
              !(m[1]!.isEmpty && m[2]!.isEmpty) else {
            throw APIError(416, "bad_range", "Range inválido")
        }
        var start: Int64, end: Int64
        if !m[1]!.isEmpty {
            guard let s = Int64(m[1]!) else { throw APIError(416, "bad_range", "Range inválido") }
            start = s
            end = m[2]!.isEmpty ? size - 1 : (Int64(m[2]!) ?? Int64.max)
        } else {
            let n = Int64(m[2]!) ?? Int64.max
            start = max(0, size - n)
            end = size - 1
        }
        end = min(end, size - 1)
        if start >= size || start > end { throw APIError(416, "bad_range", "Range fora do arquivo") }
        return (start, end)
    }

    /// Nome do console para mostrar: o que ele informou no pareamento, ou a plataforma e o IP.
    static func clientLabel(_ client: PairedClient, ip: String) -> String {
        if !client.name.isEmpty { return client.name }
        switch client.platform {
        case "ps5": return "PS5 (\(ip))"
        case "switch": return "Switch (\(ip))"
        default: return ip
        }
    }

    func sendFile(_ item: Item, index: Int, req: HTTPServer.Request, client: String = "", clientKey: String = "")
        throws -> HTTPServer.Response {
        guard index >= 0, index < item.files.count else { throw APIError(404, "not_found", "arquivo não encontrado") }
        let f = item.files[index]
        if let sent = req.header("X-Ponte-Revision"), sent.trimmingCharacters(in: .whitespaces) != item.revision {
            throw APIError(412, "revision_changed", "o item mudou; recomece a cópia")
        }
        guard let st = fileStat(f.absPath) else { throw APIError(404, "not_found", "arquivo sumiu do disco") }
        if st.size != f.size || st.mtimeNs != f.mtimeNs {
            throw APIError(412, "revision_changed", "o arquivo mudou no disco; recomece a cópia")
        }
        let size = f.size
        var status = 200
        var start: Int64 = 0, end: Int64 = size - 1
        if let range = req.header("Range") {
            (start, end) = try PonteServer.parseRange(range, size: size)
            status = 206
        }
        let length = size > 0 ? end - start + 1 : 0
        var headers: [(String, String)] = [
            ("Content-Type", "application/octet-stream"),
            ("Accept-Ranges", "bytes"),
            ("X-Ponte-Revision", item.revision),
        ]
        if let sha = library.sha256(f) { headers.append(("X-Ponte-Sha256", sha)) }
        if status == 206 { headers.append(("Content-Range", "bytes \(start)-\(end)/\(size)")) }
        var resp = HTTPServer.Response(status: status, headers: headers, body: .file(path: f.absPath, offset: start, length: length))
        // HEAD não envia corpo, então não conta como cópia (o servidor HTTP só chama os avisos com corpo)
        var hooks = TransferStream(tracker: transfers, key: TransferTracker.liveKey(itemId: item.id, client: clientKey),
                                   item: item, client: client, index: index, offset: start).hooks
        let begin = hooks.start, finish = hooks.end
        hooks.start = { [weak self] in
            self?.streamBegan()
            begin()
        }
        hooks.end = { [weak self] in
            finish()
            self?.streamEnded()
        }
        resp.transfer = hooks
        return resp
    }
}

extension String {
    /// Como `str(x)` da referência, para campos de texto livres (nil vira "").
    init(describingField v: Any?) {
        switch v {
        case nil, is NSNull: self = ""
        case let s as String: self = s
        case let n as NSNumber: self = n.stringValue
        default: self = "\(v!)"
        }
    }
}

/// Contador de threads em andamento, com espera até zerar.
final class ThreadCount: @unchecked Sendable {
    private let cond = NSCondition()
    private var n = 0

    func enter() {
        cond.lock()
        n += 1
        cond.unlock()
    }

    func leave() {
        cond.lock()
        n -= 1
        cond.broadcast()
        cond.unlock()
    }

    func waitUntilZero(timeout: TimeInterval) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        cond.lock()
        defer { cond.unlock() }
        while n > 0 {
            if !cond.wait(until: deadline) { return n == 0 }
        }
        return true
    }
}

/// Sinal de parada com espera interrompível.
final class StopFlag: @unchecked Sendable {
    private let cond = NSCondition()
    private var flag = false

    var isSet: Bool {
        cond.lock()
        defer { cond.unlock() }
        return flag
    }

    func set() {
        cond.lock()
        flag = true
        cond.broadcast()
        cond.unlock()
    }

    /// Espera até `seconds` ou até ser sinalizado.
    func wait(_ seconds: Double) {
        cond.lock()
        if !flag { _ = cond.wait(until: Date(timeIntervalSinceNow: seconds)) }
        cond.unlock()
    }
}
