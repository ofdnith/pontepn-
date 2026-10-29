import Foundation
import PonteCore
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

/// Responde à descoberta na rede (SPEC §3): datagrama "PONTE?1" na porta 47100 → JSON com
/// nome, porta e id do servidor, por unicast para quem perguntou.
public final class DiscoveryResponder: @unchecked Sendable {
    public static let query = Array("PONTE?1".utf8)

    private let fd: Int32
    private let reply: () -> Data
    private let lock = NSLock()
    private var stopped = false
    public let port: Int

    /// `reply` é chamado a cada consulta (o nome pode mudar com o app aberto).
    public init(host: String, port: Int, reply: @escaping () -> Data) throws {
        fd = try HTTPServer.bindSocket(type: Int32(HTTPServer.sockDgram), host: host, port: port)
        self.port = HTTPServer.boundPort(fd)
        self.reply = reply
        HTTPServer.setTimeout(fd, SO_RCVTIMEO, seconds: 1)
    }

    public func start() {
        let t = Thread { [weak self] in self?.loop() }
        t.name = "ponte-discovery"
        t.start()
    }

    public func stop() {
        lock.lock()
        stopped = true
        lock.unlock()
        shutdown(fd, Int32(SHUT_RDWR))
        close(fd)
    }

    private var isStopped: Bool {
        lock.lock()
        defer { lock.unlock() }
        return stopped
    }

    private func loop() {
        var buf = [UInt8](repeating: 0, count: 512)
        while !isStopped {
            var addr = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = buf.withUnsafeMutableBytes { b in
                withUnsafeMutablePointer(to: &addr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, b.baseAddress, b.count, 0, $0, &len) }
                }
            }
            if n <= 0 { continue }  // tempo esgotado, interrupção ou socket fechado
            guard DiscoveryResponder.isQuery(Array(buf[0..<n])) else { continue }
            let data = reply()
            _ = data.withUnsafeBytes { b in
                withUnsafePointer(to: &addr) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(fd, b.baseAddress, b.count, 0, $0, len) }
                }
            }
        }
    }

    /// "PONTE?1", tolerando espaços e quebras de linha nas pontas (como `data.strip()` na referência).
    static func isQuery(_ bytes: [UInt8]) -> Bool {
        let ws: Set<UInt8> = [9, 10, 11, 12, 13, 32]
        var s = bytes[...]
        while let f = s.first, ws.contains(f) { s = s.dropFirst() }
        while let l = s.last, ws.contains(l) { s = s.dropLast() }
        return Array(s) == query
    }
}
