#if os(macOS)
import AppKit
import PonteCore
import PonteServer
import SwiftUI

/// Janela da barra de menu.
struct MenuContentView: View {
    @EnvironmentObject var model: AppModel
    @State private var showSettings = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HeaderView(showSettings: $showSettings)
                .padding(14)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    if showSettings {
                        SettingsSection()
                    } else {
                        MainSections()
                    }
                }
                .padding(14)
            }
            .frame(maxHeight: 560)
            Divider()
            FooterView(showSettings: $showSettings)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
        }
        .frame(width: 380)
        .tint(Brand.accent)
    }
}

// MARK: - cabeçalho e rodapé

struct HeaderView: View {
    @EnvironmentObject var model: AppModel
    @Binding var showSettings: Bool

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Circle()
                .fill(statusColor)
                .frame(width: 9, height: 9)
                .padding(.top, 5)
            VStack(alignment: .leading, spacing: 2) {
                Text("Ponte").font(.headline)
                Text(statusText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                if case .running = model.status, let link = model.link {
                    Label(link.label, systemImage: link.kind == .wifi ? "wifi" : "cable.connector")
                        .font(.caption)
                        .foregroundStyle(link.kind == .wifi ? Brand.warning : .secondary)
                        .help(link.hint ?? "O Mac está ligado à rede por cabo.")
                }
            }
            Spacer()
            Button {
                showSettings.toggle()
            } label: {
                Image(systemName: showSettings ? "xmark.circle" : "gearshape")
            }
            .buttonStyle(.borderless)
            .help(showSettings ? "Fechar ajustes" : "Ajustes")
        }
    }

    var statusColor: Color {
        switch model.eject {
        case .ejected: return .gray
        case .ejecting: return Brand.warning
        case .idle, .waiting: break
        }
        switch model.status {
        case .running: return model.libraryMissing ? Brand.warning : Brand.success
        case .noLibrary: return .gray
        case .failed: return Brand.danger
        }
    }

    var statusText: String {
        switch model.eject {
        case .ejected(let name): return "\(name) ejetado. O Ponte volta a servir quando ele for conectado de novo."
        case .ejecting: return "Ejetando o SSD…"
        case .idle, .waiting: break
        }
        switch model.status {
        case .noLibrary:
            return "Escolha a pasta da biblioteca para começar."
        case .failed(let message):
            return message
        case .running(let addresses, let port):
            if model.libraryMissing { return "Biblioteca não encontrada. O SSD está conectado?" }
            let where_ = addresses.isEmpty ? "sem rede" : addresses.map { "\($0):\(port)" }.joined(separator: " · ")
            return "Servindo em \(where_)"
        }
    }
}

struct FooterView: View {
    @EnvironmentObject var model: AppModel
    @Binding var showSettings: Bool

    var body: some View {
        HStack {
            Button("Abrir biblioteca") { model.openLibrary() }
                .disabled(model.libraryPath.isEmpty)
            Button("Varrer agora") { model.rescan() }
                .disabled(model.libraryPath.isEmpty)
            Spacer()
            Button("Sair") { model.quit() }
        }
        .buttonStyle(.borderless)
        .controlSize(.small)
    }
}

// MARK: - seções principais

struct MainSections: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        if case .noLibrary = model.status {
            VStack(alignment: .leading, spacing: 8) {
                Text("Nenhuma biblioteca escolhida.")
                Button("Escolher pasta…") { model.chooseLibrary() }
            }
        } else {
            if model.pairingRequired && !model.code.isEmpty {
                PairingSection()
            }
            if !model.transfers.isEmpty {
                TransfersSection()
            }
            LibrarySection()
            if !model.attention.isEmpty {
                AttentionSection()
            }
            ClientsSection()
            if !model.history.isEmpty {
                HistorySection()
            }
        }
        if let error = model.lastError {
            HStack(alignment: .top) {
                Image(systemName: "exclamationmark.triangle").foregroundStyle(Brand.warning)
                Text(error).font(.caption).fixedSize(horizontal: false, vertical: true)
                Spacer()
                Button {
                    model.lastError = nil
                } label: {
                    Image(systemName: "xmark")
                }
                .buttonStyle(.borderless)
            }
        }
    }
}

struct SectionTitle: View {
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text.uppercased())
            .font(.caption2.weight(.semibold))
            .foregroundStyle(.secondary)
    }
}

struct PairingSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            SectionTitle("Código de pareamento")
            HStack(alignment: .firstTextBaseline) {
                Text(spaced(model.code))
                    .font(.system(size: 30, weight: .semibold, design: .monospaced))
                    .textSelection(.enabled)
                Spacer()
                Button("Novo código") { model.newCode() }
                    .controlSize(.small)
            }
            Text("Digite no console na primeira vez. O código muda depois de cada pareamento.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    func spaced(_ code: String) -> String {
        code.count == 6 ? "\(code.prefix(3)) \(code.suffix(3))" : code
    }
}

struct LibrarySection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            SectionTitle("Biblioteca")
            HStack(alignment: .firstTextBaseline) {
                Text(model.libraryPath)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(model.libraryPath)
                Spacer()
                if model.canEject, let volume = model.libraryVolume {
                    Button {
                        model.requestEject()
                    } label: {
                        Label("Ejetar", systemImage: "eject")
                    }
                    .controlSize(.small)
                    .help("Ejetar \(volume.name) com segurança")
                }
            }
            if model.eject != .idle {
                EjectStatusView()
            } else {
                counts
            }
        }
    }

    @ViewBuilder var counts: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 14) {
                Label("\(model.ps5Count) PS5", systemImage: "gamecontroller")
                Label("\(model.switchCount) Switch", systemImage: "gamecontroller.fill")
                if model.pendingCount > 0 {
                    Label("\(model.pendingCount) chegando", systemImage: "hourglass")
                        .foregroundStyle(.secondary)
                }
            }
            .font(.callout)
            if let job = model.job {
                PrepRow(job: job).padding(.top, 2)
            }
            if let h = model.hashing {
                ProgressRow(icon: "checkmark.shield", title: "Conferindo: \(h.title)", fraction: h.fraction,
                            status: Fmt.status(done: h.doneBytes, total: h.totalBytes, speed: h.bytesPerSecond,
                                               secondsLeft: h.secondsLeft),
                            detail: h.filesLeft == 1 ? "calculando o sha256 do último arquivo"
                                                     : "calculando o sha256 · \(h.filesLeft) arquivos na fila")
                    .padding(.top, 2)
            }
        }
    }
}

/// Extração ou conversão em andamento.
struct PrepRow: View {
    let job: Preparer.Job

    var body: some View {
        ProgressRow(icon: job.stage == .extracting ? "archivebox" : "externaldrive.badge.plus",
                    title: "\(job.stage == .extracting ? "Extraindo" : "Convertendo"): \(job.title)",
                    fraction: job.progress,
                    status: job.paused ? "pausado: um console está copiando; continua sozinho"
                                       : Fmt.status(done: job.doneBytes, total: job.totalBytes,
                                                    speed: job.bytesPerSecond, secondsLeft: job.secondsLeft),
                    detail: job.step,
                    tone: job.paused ? .paused : .running)
    }
}

/// Enquanto ejeta ou depois de ejetar o SSD.
struct EjectStatusView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        switch model.eject {
        case .idle:
            EmptyView()
        case .waiting:
            VStack(alignment: .leading, spacing: 6) {
                Label(waitingText, systemImage: "hourglass")
                    .font(.callout)
                ForEach(model.activeCopies, id: \.itemId) { copy in
                    Text("• \(copy.title)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                HStack {
                    Button("Ejetar agora") { model.ejectNow() }
                    Button("Não ejetar") { model.cancelEject() }
                }
                .controlSize(.small)
            }
        case .ejecting:
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Ejetando…").font(.callout)
            }
        case .ejected(let name):
            VStack(alignment: .leading, spacing: 4) {
                Label("\(name) foi ejetado. Pode desconectar.", systemImage: "checkmark.circle")
                    .font(.callout)
                Text("Ao conectar o SSD de novo, o Ponte volta a servir os jogos sozinho.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    var waitingText: String {
        let n = model.activeCopies.count
        if n == 0 { return "Ejetando assim que terminar…" }
        return n == 1 ? "Ejeta quando esta cópia terminar:" : "Ejeta quando estas \(n) cópias terminarem:"
    }
}

// MARK: - precisa de atenção

struct AttentionSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            SectionTitle("Precisa de atenção")
            ForEach(model.attention) { row in
                AttentionRowView(row: row)
            }
        }
    }
}

struct AttentionRowView: View {
    @EnvironmentObject var model: AppModel
    let row: AttentionRow
    @State private var expanded = false
    @State private var password = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .top) {
                Image(systemName: icon).foregroundStyle(Brand.warning)
                VStack(alignment: .leading, spacing: 1) {
                    Text(row.title).font(.callout).lineLimit(1).truncationMode(.middle)
                    Text(row.reason).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer()
                actions
            }
            if expanded {
                switch row.kind {
                case .password:
                    HStack {
                        SecureField("Senha", text: $password)
                            .textFieldStyle(.roundedBorder)
                            .onSubmit(sendPassword)
                        Button("Extrair", action: sendPassword)
                            .disabled(password.isEmpty)
                    }
                case .unidentified:
                    ManualIdentifyForm(row: row) { expanded = false }
                default:
                    EmptyView()
                }
            }
        }
    }

    @ViewBuilder var actions: some View {
        HStack(spacing: 6) {
            switch row.kind {
            case .password:
                Button("Senha…") { expanded.toggle() }
            case .unidentified:
                Button("Identificar…") { expanded.toggle() }
            case .failed:
                Button("Tentar de novo") { model.retry(row) }
            case .incomplete:
                EmptyView()
            }
            Button {
                model.reveal(row.path)
            } label: {
                Image(systemName: "magnifyingglass")
            }
            .help("Mostrar no Finder")
        }
        .buttonStyle(.borderless)
        .controlSize(.small)
    }

    var icon: String {
        switch row.kind {
        case .password: return "lock"
        case .unidentified: return "questionmark.circle"
        case .failed: return "exclamationmark.triangle"
        case .incomplete: return "puzzlepiece"
        }
    }

    func sendPassword() {
        guard !password.isEmpty else { return }
        model.submitPassword(password, for: row)
        password = ""
        expanded = false
    }
}

/// Você escolhe plataforma, tipo e Title ID; o app grava `<item>.ponte.json` (SPEC §1.6, §7.2).
struct ManualIdentifyForm: View {
    @EnvironmentObject var model: AppModel
    let row: AttentionRow
    let done: () -> Void

    @State private var platform = "ps5"
    @State private var kind = "exfat"
    @State private var content = "base"
    @State private var titleId = ""
    @State private var title = ""
    @State private var version = ""

    static let kindsByPlatform: [String: [String]] = [
        "ps5": ["exfat", "ffpkg", "ffpfs", "ffpfsc", "folder", "pkg_ps4", "pkg_ps5", "folder_ps4"],
        "switch": ["nsp", "nsz", "xci", "xcz"],
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Picker("", selection: $platform) {
                    Text("PS5").tag("ps5")
                    Text("Switch").tag("switch")
                }
                .labelsHidden()
                .frame(width: 90)
                Picker("", selection: $kind) {
                    ForEach(Self.kindsByPlatform[platform] ?? [], id: \.self) { Text($0).tag($0) }
                }
                .labelsHidden()
                Picker("", selection: $content) {
                    Text("base").tag("base")
                    Text("atualização").tag("update")
                    Text("DLC").tag("dlc")
                }
                .labelsHidden()
            }
            TextField(platform == "ps5" ? "Title ID (ex.: PPSA01234)" : "Title ID (16 dígitos hex)", text: $titleId)
            TextField("Nome do jogo", text: $title)
            TextField(platform == "ps5" ? "Versão (ex.: 01.000.000)" : "Versão (ex.: 65536)", text: $version)
            HStack {
                Spacer()
                Button("Cancelar", action: done)
                Button("Salvar") {
                    model.identifyManually(row, platform: platform, kind: kind, content: content,
                                           titleId: titleId, title: title, version: version)
                    done()
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .textFieldStyle(.roundedBorder)
        .controlSize(.small)
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.secondary.opacity(0.08)))
        .onAppear(perform: guess)
        .onChange(of: platform) { newValue in
            // só troca o tipo se o atual não valer para a nova plataforma (não desfaz o palpite do guess())
            let kinds = Self.kindsByPlatform[newValue] ?? []
            if !kinds.contains(kind) { kind = kinds.first ?? kind }
        }
    }

    /// Sugestões a partir do nome do arquivo.
    func guess() {
        let name = lastComponent(row.path)
        let ext = (name as NSString).pathExtension.lowercased()
        if let k = Identifier.switchKinds["." + ext] {
            platform = "switch"
            kind = k
            titleId = SwitchTID.tidFromName(name) ?? ""
            content = titleId.isEmpty ? "base" : SwitchTID.classify(titleId).content
            if content == "unknown" { content = "base" }
        } else {
            platform = "ps5"
            if let k = Identifier.ps5ImageKinds["." + ext] {
                kind = k
            } else if ext == "pkg" {
                kind = "pkg_ps4"
            } else if isDirectory(row.path) {
                kind = "folder"
            }
            if let m = RX(#"([A-Z]{4}\d{5})"#).search(name.uppercased()) { titleId = m[1] ?? "" }
        }
        title = SwitchTID.cleanTitle((name as NSString).deletingPathExtension)
    }
}

// MARK: - consoles e cópias

struct ClientsSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            SectionTitle("Consoles pareados")
            if model.clients.isEmpty {
                Text(model.pairingRequired ? "Nenhum ainda. Abra o Ponte no console e digite o código acima."
                                           : "Pareamento desligado: qualquer console da rede pode copiar.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            ForEach(model.clients) { c in
                HStack {
                    Image(systemName: "gamecontroller")
                    VStack(alignment: .leading, spacing: 1) {
                        Text(c.name).font(.callout)
                        Text(subtitle(c)).font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Revogar") { model.revoke(c) }
                        .buttonStyle(.borderless)
                        .controlSize(.small)
                }
            }
        }
    }

    func subtitle(_ c: ClientRow) -> String {
        let platform = c.platform == "ps5" ? "PS5" : (c.platform == "switch" ? "Switch" : c.platform)
        let fw = c.firmware.map { " · firmware \($0)" } ?? ""
        return platform + fw + " · desde " + c.pairedAt.formatted(date: .abbreviated, time: .omitted)
    }
}

// MARK: - cópias: em andamento e histórico

/// Textos e ícones das cópias, usados na janela principal, no histórico e na barra de menu.
enum CopyFormat {
    static func bytes(_ n: Int64) -> String { Fmt.bytes(n) }
    static func speed(_ bytesPerSecond: Double) -> String { Fmt.speed(bytesPerSecond) }
    static func percent(_ fraction: Double) -> String { Fmt.percent(fraction) }
    static func duration(_ seconds: Double) -> String { Fmt.duration(seconds) }
    static func timeLeft(_ seconds: Double) -> String { Fmt.timeLeft(seconds) }

    static func platformIcon(_ platform: String) -> String {
        platform == "switch" ? "gamecontroller.fill" : "gamecontroller"
    }

    static func resultText(_ r: CopyRecord) -> String {
        switch r.result {
        case "installed": return "instalado"
        case "copied": return "copiado"
        case "cancelled": return "cancelado"
        case "interrupted": return "interrompido"
        default: return "falhou"
        }
    }

    static func resultIcon(_ result: String) -> String {
        switch result {
        case "installed", "copied": return "checkmark.circle"
        case "cancelled": return "minus.circle"
        case "interrupted": return "pause.circle"
        default: return "xmark.octagon"
        }
    }

    static func resultColor(_ result: String) -> Color {
        switch result {
        case "installed", "copied": return Brand.success
        case "cancelled": return .secondary
        case "interrupted": return Brand.warning
        default: return Brand.danger
        }
    }

    /// "4,5 GB em 3 min (25 MB/s)", ou o que se souber.
    static func amount(_ r: CopyRecord) -> String {
        var s: String
        if r.sentBytes > 0 && r.sentBytes < r.totalBytes {
            s = "\(bytes(r.sentBytes)) de \(bytes(r.totalBytes))"
        } else {
            s = bytes(r.totalBytes)
        }
        if let d = r.transferSeconds ?? r.durationS.map(Double.init), d >= 1 {
            s += " em " + duration(d)
        }
        if let v = r.averageBytesPerSecond, r.sentBytes > 0 {
            s += " (\(speed(v)))"
        }
        return s
    }

    static func when(_ date: Date) -> String {
        Calendar.current.isDateInToday(date)
            ? date.formatted(date: .omitted, time: .shortened)
            : date.formatted(date: .abbreviated, time: .shortened)
    }
}

struct TransfersSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle(model.transfers.count == 1 ? "Copiando agora" : "Copiando agora (\(model.transfers.count))")
            ForEach(model.transfers) { t in
                TransferRowView(t: t)
            }
        }
    }
}

struct TransferRowView: View {
    let t: TransferSnapshot

    var body: some View {
        ProgressRow(icon: CopyFormat.platformIcon(t.platform), title: t.title, fraction: t.fraction,
                    status: status, detail: origin, detailHelp: t.currentFile,
                    tone: t.streaming || t.doneBytes >= t.totalBytes ? .running : .paused)
    }

    /// "1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min"
    var status: String {
        var parts = ["\(CopyFormat.bytes(t.doneBytes)) de \(CopyFormat.bytes(t.totalBytes))"]
        if t.streaming {
            if t.bytesPerSecond > 0 { parts.append(CopyFormat.speed(t.bytesPerSecond)) }
            if let left = t.secondsLeft { parts.append(CopyFormat.timeLeft(left)) }
        } else if t.doneBytes < t.totalBytes {
            parts.append("parado há " + CopyFormat.duration(Date().timeIntervalSince(t.lastActivity)))
        }
        return parts.joined(separator: " · ")
    }

    /// "PS5 da sala · arquivo 3 de 120: sce_sys/icon0.png"
    var origin: String {
        var parts = [t.client]
        if t.reportedResult == "installed" {
            parts.append("o console está baixando o pacote para instalar")
        } else if t.fileCount > 1, let i = t.currentFileIndex, let f = t.currentFile {
            parts.append("arquivo \(i + 1) de \(t.fileCount): \(f)")
        }
        return parts.joined(separator: " · ")
    }
}

struct HistorySection: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                SectionTitle("Histórico")
                Spacer()
                Button("Ver tudo") {
                    openWindow(id: "history")
                    NSApp.activate(ignoringOtherApps: true)
                }
                .buttonStyle(.borderless)
                .controlSize(.small)
            }
            ForEach(model.history.prefix(5)) { r in
                HStack(alignment: .top) {
                    Image(systemName: CopyFormat.resultIcon(r.result)).foregroundStyle(CopyFormat.resultColor(r.result))
                    VStack(alignment: .leading, spacing: 1) {
                        Text(r.title).font(.callout).lineLimit(1).truncationMode(.middle)
                        Text(subtitle(r)).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    }
                }
                .help(r.detail ?? "")
            }
        }
    }

    func subtitle(_ r: CopyRecord) -> String {
        var what = CopyFormat.resultText(r)
        if r.result == "failed", let d = r.detail { what += ": " + d }
        if r.result == "copied" { what += ", aguardando o montador" }
        return [what, r.client, CopyFormat.amount(r), CopyFormat.when(r.finishedAt)].joined(separator: " · ")
    }
}

/// Janela com o histórico completo de cópias.
struct HistoryWindow: View {
    @EnvironmentObject var model: AppModel
    @State private var search = ""
    @State private var confirmClear = false

    var rows: [CopyRecord] {
        let q = search.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return model.history }
        return model.history.filter {
            $0.title.localizedCaseInsensitiveContains(q) || $0.client.localizedCaseInsensitiveContains(q)
                || CopyFormat.resultText($0).localizedCaseInsensitiveContains(q)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            if model.history.isEmpty {
                VStack(spacing: 6) {
                    Image(systemName: "clock.arrow.circlepath").font(.largeTitle).foregroundStyle(.secondary)
                    Text("Nenhuma cópia ainda").font(.headline)
                    Text("As cópias feitas pelos consoles aparecem aqui.").foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                Table(rows) {
                    TableColumn("Quando") { r in
                        Text(r.finishedAt.formatted(date: .numeric, time: .shortened)).monospacedDigit()
                    }
                    .width(min: 110, ideal: 120)
                    TableColumn("Jogo") { r in
                        Text(r.title).help(r.version.map { "\(r.title) · versão \($0)" } ?? r.title)
                    }
                    .width(min: 140, ideal: 220)
                    TableColumn("Console") { r in Text(r.client) }
                        .width(min: 80, ideal: 110)
                    TableColumn("Resultado") { r in
                        Label(CopyFormat.resultText(r), systemImage: CopyFormat.resultIcon(r.result))
                            .foregroundStyle(CopyFormat.resultColor(r.result))
                            .help(r.detail ?? "")
                    }
                    .width(min: 90, ideal: 110)
                    TableColumn("Tamanho") { r in Text(CopyFormat.bytes(r.totalBytes)).monospacedDigit() }
                        .width(min: 60, ideal: 75)
                    TableColumn("Enviado") { r in Text(r.sentBytes > 0 ? CopyFormat.bytes(r.sentBytes) : "—").monospacedDigit() }
                        .width(min: 60, ideal: 75)
                    TableColumn("Duração") { r in
                        Text((r.transferSeconds ?? r.durationS.map(Double.init)).map(CopyFormat.duration) ?? "—").monospacedDigit()
                    }
                    .width(min: 55, ideal: 70)
                    TableColumn("Velocidade") { r in
                        Text(r.averageBytesPerSecond.map(CopyFormat.speed) ?? "—").monospacedDigit()
                    }
                    .width(min: 70, ideal: 85)
                    TableColumn("Destino") { r in Text(r.storage ?? "—") }
                        .width(min: 50, ideal: 60)
                }
            }
            Divider()
            HStack {
                Text(rows.count == model.history.count ? "\(rows.count) cópias" : "\(rows.count) de \(model.history.count) cópias")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Limpar histórico…") { confirmClear = true }
                    .disabled(model.history.isEmpty)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
        }
        .tint(Brand.accent)
        .searchable(text: $search, prompt: "Buscar jogo, console ou resultado")
        .frame(minWidth: 820, minHeight: 360)
        .confirmationDialog("Apagar todo o histórico de cópias?", isPresented: $confirmClear) {
            Button("Apagar", role: .destructive) { model.clearHistory() }
        } message: {
            Text("Os jogos nos consoles não são afetados.")
        }
    }
}

// MARK: - ajustes

struct SettingsSection: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 6) {
                SectionTitle("Biblioteca")
                HStack {
                    Text(model.libraryPath.isEmpty ? "Nenhuma pasta" : model.libraryPath)
                        .font(.caption)
                        .lineLimit(2)
                        .truncationMode(.middle)
                    Spacer()
                    Button("Escolher…") { model.chooseLibrary() }
                        .controlSize(.small)
                }
            }
            VStack(alignment: .leading, spacing: 6) {
                SectionTitle("Nome na rede")
                TextField(AppModel.computerName, text: $model.serverName)
                    .textFieldStyle(.roundedBorder)
            }
            VStack(alignment: .leading, spacing: 8) {
                SectionTitle("Preparo")
                Toggle("Converter pastas de PS5 em .exfat", isOn: $model.convertFolders)
                Text("Um arquivo só copia mais rápido no console, mas a conversão em si demora e ocupa espaço.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Toggle("Extrair compactados (.zip, .rar, .7z…)", isOn: $model.extractArchives)
                Text("Desligado, os compactados ficam como estão: extraia você mesmo dentro da biblioteca.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Toggle("Guardar originais em _originais/", isOn: $model.keepOriginals)
                Text("Desligado, os originais são apagados depois de extrair ou converter.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 8) {
                SectionTitle("Conferência")
                Toggle("Conferir as cópias com SHA-256", isOn: $model.verifyCopies)
                Text("Garante que o jogo chegou igual, byte por byte. Desligado, as cópias começam na hora e o console confere só o tamanho.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            VStack(alignment: .leading, spacing: 8) {
                SectionTitle("Segurança")
                Toggle("Exigir pareamento com código", isOn: $model.pairingRequired)
                Text("Desligado, qualquer aparelho da rede local pode ver e copiar a biblioteca.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            VStack(alignment: .leading, spacing: 8) {
                SectionTitle("Geral")
                Toggle("Abrir ao iniciar sessão", isOn: $model.launchAtLogin)
                Button("Abrir pasta de perfis de firmware") { model.openProfiles() }
                    .buttonStyle(.link)
            }
        }
        .toggleStyle(.switch)
        .controlSize(.small)
    }
}
#endif
