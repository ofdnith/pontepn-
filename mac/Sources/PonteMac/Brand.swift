#if os(macOS)
import AppKit
import SwiftUI

/// Cores e componentes da marca Ponte, os mesmos do payload do PS5 e do app do Switch
/// (tabela em docs/DESIGN.md). No Mac seguem o modo claro/escuro do sistema.
enum Brand {
    /// Turquesa Ponte: ações, barras de progresso, velocidade.
    static let accent = Color(light: 0x0B8794, dark: 0x3FB6C4)
    /// Concluído, instalado, servindo.
    static let success = Color(light: 0x1E9E63, dark: 0x4CC38A)
    /// Pausado, esperando, precisa de atenção.
    static let warning = Color(light: 0xB97F06, dark: 0xE5B450)
    /// Falhou.
    static let danger = Color(light: 0xD23C3C, dark: 0xEF6B6B)
    /// Fundo da trilha das barras e dos cartões.
    static let track = Color(light: 0xE2E7EE, dark: 0x273042)
    static let card = Color(light: 0xF3F5F8, dark: 0x181E29)
}

extension Color {
    /// Cor que muda com o modo claro/escuro (valores em hexadecimal, 0xRRGGBB).
    init(light: UInt32, dark: UInt32) {
        self.init(nsColor: NSColor(name: nil) { appearance in
            let isDark = appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            return NSColor(hex: isDark ? dark : light)
        })
    }
}

extension NSColor {
    convenience init(hex: UInt32) {
        self.init(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }
}

/// Barra de progresso da marca: trilha discreta, preenchimento na cor do estado e animação
/// listrada quando o total ainda não é conhecido.
struct BrandProgressBar: View {
    /// 0…1; nil = indeterminado.
    let value: Double?
    var color: Color = Brand.accent
    var height: CGFloat = 6

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Brand.track)
                if let value {
                    Capsule()
                        .fill(color)
                        .frame(width: max(height, geo.size.width * min(1, max(0, value))))
                        .animation(.easeOut(duration: 0.4), value: value)
                } else {
                    IndeterminateFill(color: color, height: height)
                        .clipShape(Capsule())
                }
            }
        }
        .frame(height: height)
        .accessibilityElement()
        .accessibilityLabel("Progresso")
        .accessibilityValue(value.map { "\(Int($0 * 100)) por cento" } ?? "em andamento")
    }
}

private struct IndeterminateFill: View {
    let color: Color
    let height: CGFloat
    @State private var phase: CGFloat = -0.35

    var body: some View {
        GeometryReader { geo in
            Capsule()
                .fill(color)
                .frame(width: geo.size.width * 0.35)
                .offset(x: geo.size.width * phase)
                .onAppear {
                    withAnimation(.linear(duration: 1.2).repeatForever(autoreverses: false)) { phase = 1 }
                }
        }
    }
}

/// Linha de progresso igual em tudo que o app mostra andando: cópias, extração, conversão e
/// hashes. Mesma anatomia no PS5 e no Switch:
///
///     [ícone] Título ........................ 37%
///     ███████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
///     1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min
///     detalhe (console, arquivo atual, etapa)
struct ProgressRow: View {
    let icon: String
    let title: String
    /// 0…1; nil = indeterminado.
    let fraction: Double?
    /// "1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min"
    let status: String
    var detail: String?
    var detailHelp: String?
    var tone: Tone = .running

    enum Tone { case running, paused, done, failed }

    var color: Color {
        switch tone {
        case .running: return Brand.accent
        case .paused: return Brand.warning
        case .done: return Brand.success
        case .failed: return Brand.danger
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: icon)
                .foregroundStyle(color)
                .frame(width: 16)
                .padding(.top, 1)
            VStack(alignment: .leading, spacing: 4) {
                HStack(alignment: .firstTextBaseline) {
                    Text(title).font(.callout).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    if let fraction {
                        Text(Fmt.percent(fraction))
                            .font(.caption.monospacedDigit().weight(.medium))
                            .foregroundStyle(color)
                    }
                }
                BrandProgressBar(value: fraction, color: color)
                if !status.isEmpty {
                    Text(status)
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                if let detail, !detail.isEmpty {
                    Text(detail)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .help(detailHelp ?? detail)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }
}

/// Textos de tamanho, velocidade e tempo, iguais em todo o app.
enum Fmt {
    static func bytes(_ n: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: n, countStyle: .file)
    }

    static func speed(_ bytesPerSecond: Double) -> String {
        bytes(Int64(bytesPerSecond)) + "/s"
    }

    static func percent(_ fraction: Double) -> String {
        "\(Int((fraction * 100).rounded(.down)))%"
    }

    static func duration(_ seconds: Double) -> String {
        let s = Int(seconds.rounded())
        if s < 60 { return "\(s) s" }
        let m = s / 60
        if m < 60 { return "\(m) min" }
        return "\(m / 60) h \(m % 60) min"
    }

    static func timeLeft(_ seconds: Double) -> String {
        if seconds < 60 { return "falta menos de 1 min" }
        return "faltam " + duration(seconds)
    }

    /// "1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min", com o que se souber.
    static func status(done: Int64, total: Int64?, speed: Double, secondsLeft: Double?) -> String {
        var parts: [String] = []
        if let total, total > 0 {
            parts.append("\(bytes(done)) de \(bytes(total))")
        } else if done > 0 {
            parts.append(bytes(done))
        }
        if speed >= 1 { parts.append(self.speed(speed)) }
        if let secondsLeft { parts.append(timeLeft(secondsLeft)) }
        return parts.joined(separator: " · ")
    }
}
#endif
