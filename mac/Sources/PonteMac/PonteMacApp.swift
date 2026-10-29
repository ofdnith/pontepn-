#if os(macOS)
import AppKit
import PonteCore
import PonteServer
import SwiftUI

@main
struct PonteMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        // Janela principal: abre ao abrir o app, como qualquer app do Mac.
        Window("Ponte", id: "main") {
            MenuContentView()
                .environmentObject(model)
        }
        .windowResizability(.contentSize)
        .defaultPosition(.center)

        // Histórico completo de cópias (aberto pelo "Ver tudo" ou pela barra de menu).
        Window("Histórico de cópias", id: "history") {
            HistoryWindow()
                .environmentObject(model)
        }
        .defaultSize(width: 900, height: 480)

        // Atalho na barra de menu, para o servidor continuar rodando com a janela fechada.
        MenuBarExtra("Ponte", systemImage: "externaldrive.connected.to.line.below") {
            MenuBarMenu()
                .environmentObject(model)
        }

        // Ajustes do app em Ponte → Ajustes… (⌘,), como em qualquer app do Mac.
        Settings {
            ScrollView { SettingsSection().padding(20) }
                .frame(width: 420, height: 420)
                .tint(Brand.accent)
                .environmentObject(model)
        }
    }
}

struct MenuBarMenu: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Button("Abrir o Ponte") {
            openWindow(id: "main")
            NSApp.activate(ignoringOtherApps: true)
        }
        if model.pairingRequired && !model.code.isEmpty {
            Text("Código de pareamento: \(model.code)")
        }
        if !model.transfers.isEmpty {
            Divider()
            ForEach(model.transfers) { t in
                Text(menuLine(t))
            }
        }
        if let job = model.job {
            Divider()
            Text(jobLine(job))
        }
        Button("Histórico de cópias…") {
            openWindow(id: "history")
            NSApp.activate(ignoringOtherApps: true)
        }
        if model.canEject, let volume = model.libraryVolume {
            Button("Ejetar \(volume.name)") { model.requestEject() }
        } else if model.eject == .waiting {
            Button("Ejetar agora") { model.ejectNow() }
            Button("Não ejetar") { model.cancelEject() }
        } else if case .ejected(let name) = model.eject {
            Text("\(name) ejetado")
        }
        Divider()
        Button("Sair do Ponte") { model.quit() }
    }

    /// "Extraindo Jogo — 37% · 120 MB/s" ou "… — pausado"
    func jobLine(_ job: Preparer.Job) -> String {
        var line = "\(job.stage == .extracting ? "Extraindo" : "Convertendo") \(job.title)"
        if let p = job.progress { line += " — \(Fmt.percent(p))" }
        if job.paused { line += " · pausado" } else if job.bytesPerSecond >= 1 { line += " · " + Fmt.speed(job.bytesPerSecond) }
        return line
    }

    /// "Jogo — 45% · 38 MB/s"
    func menuLine(_ t: TransferSnapshot) -> String {
        var line = "\(t.title) — \(CopyFormat.percent(t.fraction))"
        if t.streaming && t.bytesPerSecond > 0 { line += " · " + CopyFormat.speed(t.bytesPerSecond) }
        return line
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // app normal, com ícone no Dock (também ao rodar com `swift run`)
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    /// Fechar a janela não para o servidor: os consoles continuam copiando.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }
}
#else
@main
enum PonteMacUnsupported {
    static func main() {
        print("O app PonteMac só roda no macOS. Use o ponte-server.")
    }
}
#endif
