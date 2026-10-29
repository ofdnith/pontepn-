#if os(macOS)
import CoreWLAN
import Foundation
import PonteServer
import SystemConfiguration

/// Por onde o Mac está na rede (Wi-Fi ou cabo). Wi-Fi é a causa mais comum de cópia lenta,
/// então o app mostra isso ao lado do endereço.
struct NetworkLink: Equatable {
    enum Kind: Equatable { case wifi, wired, other }
    let kind: Kind
    /// Velocidade de transmissão do Wi-Fi, em Mbit/s (quando o sistema informa).
    let wifiMbps: Int?

    static func current() -> NetworkLink? {
        guard let main = NetworkInfo.ipv4Interfaces().first?.name else { return nil }
        var types: [String: String] = [:]
        if let all = SCNetworkInterfaceCopyAll() as? [SCNetworkInterface] {
            for i in all {
                if let bsd = SCNetworkInterfaceGetBSDName(i) as String?, let t = SCNetworkInterfaceGetInterfaceType(i) as String? {
                    types[bsd] = t
                }
            }
        }
        let type = types[main]
        if type == (kSCNetworkInterfaceTypeIEEE80211 as String) {
            let rate = CWWiFiClient.shared().interface(withName: main)?.transmitRate() ?? 0
            return NetworkLink(kind: .wifi, wifiMbps: rate > 0 ? Int(rate) : nil)
        }
        if type == (kSCNetworkInterfaceTypeEthernet as String) {
            return NetworkLink(kind: .wired, wifiMbps: nil)
        }
        return NetworkLink(kind: .other, wifiMbps: nil)
    }

    /// "Wi-Fi · 866 Mbit/s" ou "cabo de rede".
    var label: String {
        switch kind {
        case .wifi: return "Wi-Fi" + (wifiMbps.map { " · \($0) Mbit/s" } ?? "")
        case .wired: return "cabo de rede"
        case .other: return "rede"
        }
    }

    /// Dica quando a rede pode ser o limite da cópia.
    var hint: String? {
        kind == .wifi ? "No Wi-Fi a cópia fica limitada pelo sinal. Com cabo de rede no Mac (e no console) costuma ser bem mais rápida." : nil
    }
}
#endif
