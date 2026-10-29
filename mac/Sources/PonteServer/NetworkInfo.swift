import Foundation
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

/// Endereços IPv4 do Mac na rede local, para mostrar no app (e digitar no console, se a
/// descoberta não funcionar).
public enum NetworkInfo {
    public static func ipv4Addresses() -> [String] {
        ipv4Interfaces().map(\.ip).reduce(into: [String]()) { if !$0.contains($1) { $0.append($1) } }
    }

    /// Interfaces com IPv4 (nome BSD, ex.: "en0", e endereço), a principal primeiro.
    public static func ipv4Interfaces() -> [(name: String, ip: String)] {
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0, let first = head else { return [] }
        defer { freeifaddrs(head) }
        var found: [(name: String, ip: String)] = []
        var cursor: UnsafeMutablePointer<ifaddrs>? = first
        while let ifa = cursor {
            defer { cursor = ifa.pointee.ifa_next }
            let flags = Int32(ifa.pointee.ifa_flags)
            guard let addr = ifa.pointee.ifa_addr, addr.pointee.sa_family == sa_family_t(AF_INET),
                  flags & Int32(IFF_UP) != 0, flags & Int32(IFF_LOOPBACK) == 0 else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            let len = socklen_t(MemoryLayout<sockaddr_in>.size)
            guard getnameinfo(addr, len, &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 else { continue }
            let ip = String(cString: host)
            if ip.hasPrefix("169.254.") { continue }  // sem DHCP
            found.append((String(cString: ifa.pointee.ifa_name), ip))
        }
        // en0 (Wi-Fi/Ethernet principal) primeiro
        return found.sorted { a, b in
            if (a.name == "en0") != (b.name == "en0") { return a.name == "en0" }
            return a.name < b.name
        }
    }
}
