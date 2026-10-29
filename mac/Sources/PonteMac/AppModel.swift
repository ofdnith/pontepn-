#if os(macOS)
import AppKit
import Foundation
import PonteCore
import PonteServer
import ServiceManagement

/// Linha de "precisa de atenção": item não identificado, compactado com senha, falha no preparo…
struct AttentionRow: Identifiable, Equatable {
    enum Kind: Equatable {
        /// Não deu para identificar: você escolhe plataforma, tipo e Title ID (grava o arquivo lateral).
        case unidentified
        case password
        case failed
        case incomplete
    }

    let id: String
    let title: String
    let reason: String
    let path: String
    let kind: Kind
}

struct ClientRow: Identifiable, Equatable {
    let id: String  // token
    let name: String
    let platform: String
    let firmware: String?
    let pairedAt: Date
}

/// Disco onde está a biblioteca, quando dá para ejetar (SSD externo, pendrive…).
struct LibraryVolume: Equatable {
    let url: URL
    let name: String
}

/// Estado do app: configurações (UserDefaults), o servidor e um retrato atualizado a cada segundo
/// para a interface.
@MainActor
final class AppModel: ObservableObject {
    enum Status: Equatable {
        case noLibrary
        case running(addresses: [String], port: Int)
        case failed(String)
    }

    // MARK: configurações

    @Published var libraryPath: String {
        didSet {
            defaults.set(libraryPath, forKey: "libraryPath")
            restart()
        }
    }

    @Published var serverName: String {
        didSet {
            defaults.set(serverName, forKey: "serverName")
            server?.name = effectiveName
        }
    }

    @Published var pairingRequired: Bool {
        didSet {
            defaults.set(pairingRequired, forKey: "pairingRequired")
            server?.pairingRequired = pairingRequired
        }
    }

    @Published var convertFolders: Bool {
        didSet {
            defaults.set(convertFolders, forKey: "convertFolders")
            server?.preparer.convertFolders = convertFolders
            server?.library.holdFolders = convertFolders
            server?.rescan()
        }
    }

    @Published var extractArchives: Bool {
        didSet {
            defaults.set(extractArchives, forKey: "extractArchives")
            server?.preparer.extractArchives = extractArchives
        }
    }

    /// Conferir as cópias com SHA-256. Desligado (padrão), as cópias começam na hora e o Mac não
    /// passa tempo lendo a biblioteca inteira; o console confere só o tamanho.
    @Published var verifyCopies: Bool {
        didSet {
            defaults.set(verifyCopies, forKey: "verifyCopies")
            server?.library.hashesEnabled = verifyCopies
        }
    }

    @Published var keepOriginals: Bool {
        didSet {
            defaults.set(keepOriginals, forKey: "keepOriginals")
            server?.preparer.keepOriginals = keepOriginals
        }
    }

    @Published var launchAtLogin: Bool {
        didSet {
            guard launchAtLogin != (SMAppService.mainApp.status == .enabled) else { return }
            do {
                if launchAtLogin { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
            } catch {
                lastError = "Abrir ao iniciar sessão: \(error.localizedDescription)"
            }
        }
    }

    // MARK: retrato para a interface

    @Published private(set) var status: Status = .noLibrary
    @Published private(set) var code = ""
    @Published private(set) var ps5Count = 0
    @Published private(set) var switchCount = 0
    @Published private(set) var pendingCount = 0
    @Published private(set) var job: Preparer.Job?
    /// Cálculo de hashes em andamento (sha256 de cada arquivo, em segundo plano).
    @Published private(set) var hashing: Library.HashProgress?
    @Published private(set) var attention: [AttentionRow] = []
    @Published private(set) var clients: [ClientRow] = []
    /// Cópias em andamento (progresso, velocidade, arquivo atual).
    @Published private(set) var transfers: [TransferSnapshot] = []
    /// Cópias terminadas, mais recentes primeiro. Fica visível mesmo com o SSD ejetado.
    @Published private(set) var history: [CopyRecord] = []
    @Published private(set) var libraryMissing = false
    /// Wi-Fi ou cabo (atualizado a cada 10 s).
    @Published private(set) var link: NetworkLink?
    private var linkCheckedAt = Date.distantPast
    @Published var lastError: String?

    // MARK: ejetar o SSD

    enum EjectState: Equatable {
        case idle
        /// Esperando as cópias em andamento terminarem para ejetar.
        case waiting
        case ejecting
        /// Ejetado; o servidor volta sozinho quando o disco for conectado de novo.
        case ejected(String)
    }

    @Published private(set) var eject: EjectState = .idle
    @Published private(set) var libraryVolume: LibraryVolume?
    @Published private(set) var activeCopies: [ActiveTransfer] = []

    var canEject: Bool { libraryVolume != nil && server != nil && eject == .idle }
    private var mountObserver: NSObjectProtocol?
    /// Ejetar pelo Finder (ou qualquer outro lugar): solta o disco antes de o macOS desmontar.
    private let diskWatcher = DiskWatcher()

    private let defaults = UserDefaults.standard
    private var server: PonteServer?
    private var timer: Timer?

    let supportDir: String
    var profilesDir: String { joinPath(supportDir, "profiles") }

    init() {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("Ponte").path
        supportDir = support
        libraryPath = defaults.string(forKey: "libraryPath") ?? ""
        serverName = defaults.string(forKey: "serverName") ?? ""
        pairingRequired = defaults.object(forKey: "pairingRequired") as? Bool ?? true
        // Conversão para .exfat, conferência por SHA-256 e extração de compactados vêm desligadas
        // (deixam tudo lento; extrair fica por sua conta). Ajustes salvos por versões de teste
        // anteriores (settingsVersion < 3) também passam a desligado, uma vez.
        if defaults.integer(forKey: "settingsVersion") < 3 {
            defaults.set(false, forKey: "convertFolders")
            defaults.set(false, forKey: "verifyCopies")
            defaults.set(false, forKey: "extractArchives")
            defaults.set(3, forKey: "settingsVersion")
        }
        convertFolders = defaults.object(forKey: "convertFolders") as? Bool ?? false
        verifyCopies = defaults.object(forKey: "verifyCopies") as? Bool ?? false
        extractArchives = defaults.object(forKey: "extractArchives") as? Bool ?? false
        keepOriginals = defaults.object(forKey: "keepOriginals") as? Bool ?? true
        launchAtLogin = SMAppService.mainApp.status == .enabled
        seedProfiles()
        restart()
        diskWatcher?.release = { [weak self] in
            // na fila do Disk Arbitration: tira o servidor de cena no laço principal e espera ele
            // soltar os arquivos aqui, sem travar a interface
            var s: PonteServer?
            DispatchQueue.main.sync { s = MainActor.assumeIsolated { self?.releaseForExternalEject() } }
            s?.stopAndWait(timeout: 6)
        }
        // SSD conectado de novo depois de ejetar: volta a servir
        mountObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didMountNotification, object: nil, queue: .main
        ) { [weak self] _ in
            guard let model = self else { return }
            Task { @MainActor in model.volumeMounted() }
        }
        // o Timer roda no laço principal; o Task só leva o trabalho para o MainActor
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let model = self else { return }
            Task { @MainActor in model.refresh() }
        }
    }

    /// Lido uma vez: `Host.current()` pode demorar, e a tela de ajustes é redesenhada a cada segundo.
    static let computerName = Host.current().localizedName ?? "Mac"

    var effectiveName: String {
        let n = serverName.trimmingCharacters(in: .whitespaces)
        return n.isEmpty ? AppModel.computerName : n
    }

    /// Perfis: os do pacote do app (Resources/profiles) ou os embutidos, copiados uma vez para a
    /// pasta do app, onde você pode editar.
    private func seedProfiles() {
        let fm = FileManager.default
        try? fm.createDirectory(atPath: profilesDir, withIntermediateDirectories: true)
        if let bundled = Bundle.main.resourcePath.map({ joinPath($0, "profiles") }),
           let names = try? fm.contentsOfDirectory(atPath: bundled) {
            for n in names where n.hasSuffix(".json") && !fm.fileExists(atPath: joinPath(profilesDir, n)) {
                try? fm.copyItem(atPath: joinPath(bundled, n), toPath: joinPath(profilesDir, n))
            }
        }
        Profiles.seed(directory: profilesDir)
    }

    // MARK: servidor

    func restart() {
        server?.stop()
        server = nil
        eject = .idle
        guard !libraryPath.isEmpty else {
            status = .noLibrary
            refresh()
            return
        }
        var config = PonteConfig(library: libraryPath, profilesDir: profilesDir, stateDir: supportDir, name: effectiveName)
        config.pairing = pairingRequired
        config.convertFolders = convertFolders
        config.computeHashes = verifyCopies
        config.extractArchives = extractArchives
        config.keepOriginals = keepOriginals
        do {
            let s = try PonteServer(config: config, log: { line in NSLog("%@", line) })
            try s.start()
            server = s
            status = .running(addresses: NetworkInfo.ipv4Addresses(), port: s.port)
        } catch {
            status = .failed("Não foi possível abrir a porta \(config.port): \(error.localizedDescription)")
        }
        refresh()
    }

    func refresh() {
        let volume = isDirectory(libraryPath) ? AppModel.ejectableVolume(for: libraryPath) : nil
        if volume != libraryVolume {
            libraryVolume = volume
            diskWatcher?.watch(volume: volume?.url)
        }
        let copies = server?.activeTransfers() ?? []
        if copies != activeCopies { activeCopies = copies }
        if eject == .waiting, copies.isEmpty, server?.preparer.current == nil, let volume {
            performEject(volume)
        }
        guard let s = server else {
            code = ""
            ps5Count = 0
            switchCount = 0
            pendingCount = 0
            job = nil
            hashing = nil
            attention = []
            clients = []
            transfers = []
            return
        }
        if case .running(_, let port) = status {
            let addrs = NetworkInfo.ipv4Addresses()
            if status != .running(addresses: addrs, port: port) { status = .running(addresses: addrs, port: port) }
        }
        libraryMissing = !isDirectory(libraryPath)
        if Date().timeIntervalSince(linkCheckedAt) >= 10 {
            linkCheckedAt = Date()
            let l = NetworkLink.current()
            if l != link { link = l }
        }
        code = s.code
        let items = s.library.items()
        ps5Count = items.filter { $0.platform == "ps5" }.count
        switchCount = items.filter { $0.platform == "switch" }.count
        pendingCount = s.library.pending.count + s.library.toConvert.count
        job = s.preparer.current
        hashing = s.hashing

        var rows: [AttentionRow] = s.library.attention.map {
            AttentionRow(id: $0.path, title: lastComponent($0.path), reason: $0.reason, path: $0.path, kind: .unidentified)
        }
        for p in s.preparer.problems {
            let kind: AttentionRow.Kind = p.needsPassword ? .password
                : (p.reason.hasPrefix("conjunto incompleto") ? .incomplete : .failed)
            rows.append(AttentionRow(id: "prep:" + p.key, title: p.title, reason: p.reason, path: p.key, kind: kind))
        }
        if rows != attention { attention = rows }

        let c = s.pairedClients.map {
            ClientRow(id: $0.token, name: $0.client.name.isEmpty ? "Console" : $0.client.name, platform: $0.client.platform,
                      firmware: $0.client.firmware, pairedAt: Date(timeIntervalSince1970: Double($0.client.pairedAt)))
        }
        if c != clients { clients = c }
        let t = s.currentTransfers
        if t != transfers { transfers = t }
        let h = s.history
        if h != history { history = h }
    }

    // MARK: ações

    func chooseLibrary() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Usar esta pasta"
        panel.message = "Escolha a pasta da biblioteca de jogos (por exemplo, no SSD externo)"
        NSApp.activate(ignoringOtherApps: true)
        if panel.runModal() == .OK, let url = panel.url {
            libraryPath = url.path
        }
    }

    func clearHistory() {
        server?.clearHistory()
        refresh()
    }

    func newCode() {
        server?.rotateCode()
        refresh()
    }

    func revoke(_ client: ClientRow) {
        server?.revoke(token: client.id)
        refresh()
    }

    func rescan() {
        server?.rescan()
        refresh()
    }

    func retry(_ row: AttentionRow) {
        server?.preparer.retry(row.path)
        refresh()
    }

    func submitPassword(_ password: String, for row: AttentionRow) {
        server?.preparer.setPassword(password, for: row.path)
        refresh()
    }

    /// Identificação manual: grava o arquivo lateral (SPEC §1.6) e varre de novo.
    func identifyManually(_ row: AttentionRow, platform: String, kind: String, content: String,
                          titleId: String, title: String, version: String) {
        func clean(_ s: String) -> String? {
            let t = s.trimmingCharacters(in: .whitespaces)
            return t.isEmpty ? nil : t
        }
        let tid = clean(titleId)?.uppercased()
        var parent: String?
        if platform == "switch", let tid { parent = SwitchTID.classify(tid).parent }
        let ident = Identified(platform: platform, kind: kind, content: content, title: clean(title),
                               titleId: tid, version: clean(version), parentTitleId: parent)
        do {
            try Identifier.writeSidecar(ident, for: row.path)
            rescan()
        } catch {
            lastError = "Não foi possível gravar \(lastComponent(row.path)).ponte.json: \(error.localizedDescription)"
        }
    }

    func reveal(_ path: String) {
        // compactados divididos são identificados pelo nome do conjunto, que pode não existir
        let target = FileManager.default.fileExists(atPath: path) ? path : parentPath(path)
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: target)])
    }

    func openLibrary() {
        guard !libraryPath.isEmpty else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: libraryPath))
    }

    func openProfiles() {
        NSWorkspace.shared.open(URL(fileURLWithPath: profilesDir))
    }

    /// Volume da biblioteca, se for um disco que dá para ejetar (não o disco interno do Mac).
    static func ejectableVolume(for path: String) -> LibraryVolume? {
        let url = URL(fileURLWithPath: path)
        guard let vol = (try? url.resourceValues(forKeys: [.volumeURLKey]))?.volume else { return nil }
        let keys: Set<URLResourceKey> = [.volumeNameKey, .volumeIsEjectableKey, .volumeIsRemovableKey,
                                         .volumeIsInternalKey, .volumeIsRootFileSystemKey]
        guard let v = try? vol.resourceValues(forKeys: keys), v.volumeIsRootFileSystem != true else { return nil }
        guard v.volumeIsEjectable == true || v.volumeIsRemovable == true || v.volumeIsInternal == false else { return nil }
        return LibraryVolume(url: vol, name: v.volumeName ?? vol.lastPathComponent)
    }

    /// Botão "Ejetar": sem cópias nem preparo em andamento, ejeta direto; senão pergunta.
    func requestEject() {
        guard let volume = libraryVolume, let s = server, eject == .idle else { return }
        let copies = s.activeTransfers()
        let job = s.preparer.current
        if copies.isEmpty && job == nil { return performEject(volume) }

        let alert = NSAlert()
        alert.alertStyle = .warning
        if copies.isEmpty {
            alert.messageText = "Ejetar \(volume.name)?"
        } else {
            alert.messageText = copies.count == 1 ? "Há uma cópia em andamento" : "Há \(copies.count) cópias em andamento"
        }
        var parts: [String] = []
        if !copies.isEmpty {
            var lines = copies.prefix(5).map { "• \($0.title)" }
            if copies.count > 5 { lines.append("• e mais \(copies.count - 5)") }
            parts.append("Um console está copiando do \(volume.name):\n" + lines.joined(separator: "\n")
                + "\n\nSe ejetar agora, a cópia para; o console retoma de onde parou quando o SSD voltar.")
        }
        if let job {
            parts.append("\(job.stage.rawValue.capitalized) \(job.title): isso é interrompido agora e recomeça do zero quando o SSD voltar.")
        }
        alert.informativeText = parts.joined(separator: "\n\n")
        if !copies.isEmpty { alert.addButton(withTitle: "Esperar terminar") }
        alert.addButton(withTitle: "Ejetar agora")
        alert.addButton(withTitle: "Cancelar")
        NSApp.activate(ignoringOtherApps: true)
        let answer = alert.runModal()
        let first = NSApplication.ModalResponse.alertFirstButtonReturn
        let second = NSApplication.ModalResponse.alertSecondButtonReturn
        if copies.isEmpty {
            if answer == first { performEject(volume) }
        } else if answer == first {
            // não começa preparos novos (e interrompe o atual); ejeta quando as cópias acabarem
            eject = .waiting
            server?.preparer.cancel()
        } else if answer == second {
            performEject(volume)
        }
    }

    /// Enquanto espera: ejetar sem esperar mais.
    func ejectNow() {
        guard eject == .waiting, let volume = libraryVolume else { return }
        performEject(volume)
    }

    /// Enquanto espera: desistir de ejetar.
    func cancelEject() {
        guard eject == .waiting else { return }
        eject = .idle
        server?.preparer.resume()
    }

    private func performEject(_ volume: LibraryVolume) {
        eject = .ejecting
        let s = server
        server = nil
        refresh()
        Task.detached { [weak self] in
            // solta o disco: fecha as conexões, interrompe o preparo, para varredura e hashes
            s?.stopAndWait(timeout: 15)
            var failure: String?
            for attempt in 0..<4 {
                failure = AppModel.ejectDisk(volume.url.path)
                if failure == nil { break }
                if attempt < 3 { try? await Task.sleep(nanoseconds: 1_500_000_000) }
            }
            let message = failure
            await self?.finishEject(volume, failure: message)
        }
    }

    private func finishEject(_ volume: LibraryVolume, failure: String?) {
        if let failure {
            restart()
            lastError = "Não foi possível ejetar \(volume.name): \(failure)"
        } else {
            eject = .ejected(volume.name)
            libraryVolume = nil
            activeCopies = []
            NSLog("Ponte: %@ ejetado", volume.name)
        }
        refresh()
    }

    /// `diskutil eject`: desmonta todos os volumes do disco e o ejeta. nil = deu certo.
    nonisolated static func ejectDisk(_ volumePath: String) -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/diskutil")
        p.arguments = ["eject", volumePath]
        p.standardInput = FileHandle.nullDevice
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        do {
            try p.run()
        } catch {
            return error.localizedDescription
        }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        if p.terminationStatus == 0 { return nil }
        let out = String(decoding: data, as: UTF8.self)
        // "dissented by PID 123 (/Applications/X.app/...)": diz quem está segurando o disco
        if let m = RX(#"dissented by PID (\d+)(?: \(([^)]+)\))?"#).search(out) {
            let who = m[2].map { lastComponent($0) } ?? "processo \(m[1]!)"
            return "o disco está em uso por \(who). Feche-o e tente de novo."
        }
        let line = out.split(separator: "\n").last.map(String.init) ?? "erro \(p.terminationStatus)"
        return line + " Feche o que estiver usando o disco (Finder, Terminal…) e tente de novo."
    }

    private func volumeMounted() {
        guard case .ejected = eject, isDirectory(libraryPath) else { return }
        restart()
        libraryVolume = AppModel.ejectableVolume(for: libraryPath)
        diskWatcher?.watch(volume: libraryVolume?.url)
    }

    /// Alguém fora do app pediu para desmontar o SSD. Devolve o servidor para ser parado (fora do
    /// laço principal); nil quando não há o que soltar (ex.: o próprio botão Ejetar já parou tudo).
    func releaseForExternalEject() -> PonteServer? {
        guard let s = server, eject == .idle || eject == .waiting else { return nil }
        let name = libraryVolume?.name ?? lastComponent(libraryPath)
        server = nil
        eject = .ejected(name)
        activeCopies = []
        NSLog("Ponte: pedido para ejetar %@; soltando o disco", name)
        // se o disco não sair (outro programa recusou), volta a servir
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 20_000_000_000)
            self?.recoverIfStillMounted()
        }
        return s
    }

    private func recoverIfStillMounted() {
        guard case .ejected = eject, isDirectory(libraryPath),
              AppModel.ejectableVolume(for: libraryPath) != nil else { return }
        NSLog("Ponte: o disco não foi ejetado; voltando a servir")
        restart()
    }

    func quit() {
        server?.stop()
        NSApp.terminate(nil)
    }
}
#endif
