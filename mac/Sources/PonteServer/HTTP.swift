import Foundation
import PonteCore
#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif

/// Servidor HTTP/1.1 mínimo sobre sockets POSIX, só com o que o protocolo Ponte usa (SPEC §2):
/// keep-alive, `Content-Length` sempre (sem chunked), corpo de arquivo enviado em pedaços.
/// Uma thread por conexão: são poucos consoles, e assim o envio de arquivos grandes fica simples.
public final class HTTPServer: @unchecked Sendable {
    public struct Request {
        public let method: String
        /// Alvo cru da requisição, ex.: "/api/v1/catalog?platform=ps5".
        public let target: String
        /// Cabeçalhos com nome em minúsculas.
        public let headers: [String: String]
        public let body: Data
        public let remoteIP: String
        /// O corpo passou do limite e foi descartado (a conexão será fechada).
        public let bodyTooLarge: Bool

        public func header(_ name: String) -> String? { headers[name.lowercased()] }

        public var path: String {
            String(target.split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false).first ?? "")
        }

        /// Primeiro valor de cada parâmetro da query, decodificado.
        public var query: [String: String] {
            let parts = target.split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2 else { return [:] }
            var out: [String: String] = [:]
            for pair in parts[1].split(separator: "&") {
                let kv = pair.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
                let key = HTTPServer.unescape(String(kv[0]))
                let value = kv.count > 1 ? HTTPServer.unescape(String(kv[1])) : ""
                if out[key] == nil && !value.isEmpty { out[key] = value }
            }
            return out
        }
    }

    public enum Body {
        case none
        case data(Data)
        /// Trecho de um arquivo: caminho, início e tamanho. O arquivo é aberto antes de enviar os
        /// cabeçalhos; se não abrir, a resposta vira 404.
        case file(path: String, offset: Int64, length: Int64)
    }

    public struct Response {
        public var status: Int
        public var headers: [(String, String)]
        public var body: Body
        /// Para HEAD: mesmos cabeçalhos, sem corpo.
        public var headOnly = false
        /// Força fechar a conexão depois de responder.
        public var close = false
        /// Avisos do envio de um arquivo (para medir cópias em andamento).
        public var transfer: TransferHooks?

        public init(status: Int, headers: [(String, String)] = [], body: Body = .none) {
            self.status = status
            self.headers = headers
            self.body = body
        }
    }

    public typealias Handler = (Request) -> Response

    /// Chamados durante o envio de um corpo `.file`: `start` quando o corpo começa a sair, `sent`
    /// a cada pedaço enviado (bytes do pedaço) e `end` ao terminar, completo ou não.
    public struct TransferHooks {
        public var start: () -> Void
        public var sent: (Int64) -> Void
        public var end: () -> Void

        public init(start: @escaping () -> Void, sent: @escaping (Int64) -> Void, end: @escaping () -> Void) {
            self.start = start
            self.sent = sent
            self.end = end
        }
    }

    static let maxHeaderBytes = 32 * 1024
    public static let maxBody = 1 << 20
    /// Leitura por vez no caminho sem `sendfile`.
    static let sendChunk = 4 << 20
    /// Quanto o `sendfile` manda por chamada (o progresso é contado a cada uma).
    static let sendfileChunk = 8 << 20
    /// Buffer de envio do socket: com o padrão, uma rede rápida fica esperando o Mac encher o buffer.
    static let socketSendBuffer: Int32 = 4 << 20
    static let idleTimeout = 120

    private let handler: Handler
    private var listenFD: Int32 = -1
    private let lock = NSLock()
    private var connections = Set<Int32>()
    private var stopped = false
    public private(set) var port: Int = 0

    public init(host: String, port: Int, handler: @escaping Handler) throws {
        self.handler = handler
        signal(SIGPIPE, SIG_IGN)
        listenFD = try HTTPServer.bindSocket(type: Int32(HTTPServer.sockStream), host: host, port: port)
        guard listen(listenFD, 64) == 0 else {
            let e = errno
            close(listenFD)
            throw POSIXError(POSIXErrorCode(rawValue: e) ?? .EIO)
        }
        self.port = HTTPServer.boundPort(listenFD)
    }

    public func start() {
        let t = Thread { [weak self] in self?.acceptLoop() }
        t.name = "ponte-http-accept"
        t.start()
    }

    public func stop() {
        lock.lock()
        stopped = true
        let fds = connections
        lock.unlock()
        shutdown(listenFD, Int32(SHUT_RDWR))
        close(listenFD)
        for fd in fds { shutdown(fd, Int32(SHUT_RDWR)) }
    }

    /// Depois de `stop()`: espera todas as conexões fecharem (e soltarem os arquivos abertos).
    public func waitUntilClosed(timeout: TimeInterval) -> Bool {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while true {
            lock.lock()
            let open = connections.count
            lock.unlock()
            if open == 0 { return true }
            if Date() >= deadline { return false }
            usleep(50_000)
        }
    }

    private func acceptLoop() {
        while true {
            var addr = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let fd = withUnsafeMutablePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { accept(listenFD, $0, &len) }
            }
            if fd < 0 {
                if errno == EINTR { continue }
                lock.lock()
                let done = stopped
                lock.unlock()
                if done { return }
                usleep(10_000)
                continue
            }
            let ip = HTTPServer.ipString(addr)
            lock.lock()
            if stopped {
                lock.unlock()
                close(fd)
                return
            }
            connections.insert(fd)
            lock.unlock()
            HTTPServer.configureClientSocket(fd)
            let t = Thread { [weak self] in
                self?.serve(fd: fd, ip: ip)
                close(fd)
                self?.lock.lock()
                self?.connections.remove(fd)
                self?.lock.unlock()
            }
            t.name = "ponte-http-conn"
            // envio de arquivos é o que importa aqui: a prioridade mais alta, para o macOS não segurar
            // nem a CPU nem a leitura do disco (preparo, varredura e hashes rodam em prioridade baixa)
            t.qualityOfService = .userInteractive
            t.start()
        }
    }

    // MARK: conexão

    private func serve(fd: Int32, ip: String) {
        let conn = Connection(fd: fd)
        while true {
            guard let head = conn.readHead() else { return }
            guard let parsed = HTTPServer.parseHead(head) else {
                _ = send(fd, HTTPServer.errorResponse(400), keepAlive: false)
                return
            }
            var keepAlive = parsed.version == "HTTP/1.1"
            if let c = parsed.headers["connection"]?.lowercased() {
                if c.contains("close") { keepAlive = false } else if c.contains("keep-alive") { keepAlive = true }
            }
            if parsed.headers["transfer-encoding"] != nil {
                _ = send(fd, HTTPServer.errorResponse(400), keepAlive: false)
                return
            }
            var length = 0
            if let cl = parsed.headers["content-length"] {
                guard let n = Int(cl.trimmingCharacters(in: .whitespaces)), n >= 0 else {
                    _ = send(fd, HTTPServer.errorResponse(400), keepAlive: false)
                    return
                }
                length = n
            }
            var body = Data()
            let tooLarge = length > HTTPServer.maxBody
            if tooLarge {
                keepAlive = false
            } else if length > 0 {
                guard let b = conn.readBody(length) else { return }
                body = b
            }
            let req = Request(method: parsed.method, target: parsed.target, headers: parsed.headers, body: body,
                              remoteIP: ip, bodyTooLarge: tooLarge)
            var resp = handler(req)
            if req.method == "HEAD" { resp.headOnly = true }
            if resp.close { keepAlive = false }
            guard send(fd, resp, keepAlive: keepAlive), keepAlive else { return }
        }
    }

    /// Envia a resposta. false = a conexão deve ser fechada.
    private func send(_ fd: Int32, _ resp: Response, keepAlive: Bool) -> Bool {
        var resp = resp
        var fileFD: Int32 = -1
        var fileOffset: Int64 = 0
        var length: Int64 = 0
        switch resp.body {
        case .none: length = 0
        case .data(let d): length = Int64(d.count)
        case .file(let path, let offset, let len):
            if len > 0 && !resp.headOnly {
                let f = open(path, O_RDONLY)
                guard f >= 0 else {
                    return send(fd, HTTPServer.jsonError(404, "not_found", "arquivo sumiu do disco"), keepAlive: keepAlive)
                }
                HTTPServer.adviseSequentialRead(f, offset: offset, length: len)
                fileFD = f
                fileOffset = offset
            }
            length = len
        }
        defer { if fileFD >= 0 { close(fileFD) } }
        resp.headers.removeAll { $0.0.lowercased() == "content-length" || $0.0.lowercased() == "connection" }
        var head = "HTTP/1.1 \(resp.status) \(HTTPServer.reason(resp.status))\r\n"
        for (k, v) in resp.headers { head += "\(k): \(v)\r\n" }
        head += "Content-Length: \(length)\r\n"
        head += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n"
        head += "\r\n"
        guard writeAll(fd, Data(head.utf8)) else { return false }
        if resp.headOnly { return true }
        switch resp.body {
        case .none: return true
        case .data(let d): return writeAll(fd, d)
        case .file:
            guard fileFD >= 0 else { return true }
            resp.transfer?.start()
            defer { resp.transfer?.end() }
            return sendFileBody(socket: fd, file: fileFD, offset: fileOffset, length: length) { resp.transfer?.sent($0) }
        }
    }

    /// Corpo de arquivo. No macOS vai por `sendfile`: o kernel lê do disco e escreve no socket sem
    /// passar pelo app (sem cópias, e o disco lê adiantado enquanto a rede envia). Se o sistema não
    /// aceitar (`ENOTSUP` etc.) ou fora do macOS, cai na leitura com `pread` num buffer reutilizado.
    private func sendFileBody(socket fd: Int32, file: Int32, offset: Int64, length: Int64,
                              sent: (Int64) -> Void) -> Bool {
        var pos = offset
        var remaining = length
        #if os(macOS)
        while remaining > 0 {
            var len = off_t(min(Int64(HTTPServer.sendfileChunk), remaining))
            let rc = Darwin.sendfile(file, fd, off_t(pos), &len, nil, 0)
            let n = Int64(len)
            if n > 0 {
                pos += n
                remaining -= n
                sent(n)
            }
            if rc == 0 {
                if n == 0 { return false }  // fim do arquivo antes da hora: encolheu no meio do envio
                continue
            }
            let e = errno
            if e == EINTR || (e == EAGAIN && n > 0) { continue }
            if e == ENOTSUP || e == EOPNOTSUPP || e == EINVAL || e == ENOTSOCK { break }  // usa o caminho comum
            return false
        }
        if remaining == 0 { return true }
        #endif
        var buffer = [UInt8](repeating: 0, count: HTTPServer.sendChunk)
        while remaining > 0 {
            let want = Int(min(Int64(buffer.count), remaining))
            let got = buffer.withUnsafeMutableBytes { pread(file, $0.baseAddress, want, off_t(pos)) }
            if got < 0 && errno == EINTR { continue }
            if got <= 0 { return false }  // erro de leitura, ou o arquivo encolheu no meio do envio
            let ok = buffer.withUnsafeBytes { raw -> Bool in
                writeAll(fd, UnsafeRawBufferPointer(rebasing: raw[0..<got]))
            }
            guard ok else { return false }
            pos += Int64(got)
            remaining -= Int64(got)
            sent(Int64(got))
        }
        return true
    }

    /// Avisa o sistema que o arquivo vai ser lido em sequência a partir de `offset`, para ele ler
    /// adiantado (o SSD externo rende muito mais com leituras grandes e contínuas).
    static func adviseSequentialRead(_ fd: Int32, offset: Int64, length: Int64) {
        #if os(macOS)
        _ = fcntl(fd, F_RDAHEAD, 1)
        var ra = radvisory(ra_offset: off_t(offset), ra_count: Int32(clamping: min(length, Int64(64 << 20))))
        _ = fcntl(fd, F_RDADVISE, &ra)
        #elseif os(Linux)
        _ = posix_fadvise(fd, off_t(offset), off_t(length), POSIX_FADV_SEQUENTIAL)
        #endif
    }

    private func writeAll(_ fd: Int32, _ data: Data) -> Bool {
        data.withUnsafeBytes { writeAll(fd, $0) }
    }

    private func writeAll(_ fd: Int32, _ raw: UnsafeRawBufferPointer) -> Bool {
        do {
            guard var p = raw.baseAddress else { return true }
            var left = raw.count
            while left > 0 {
                #if os(Linux)
                let n = Glibc.send(fd, p, left, Int32(MSG_NOSIGNAL))
                #else
                let n = Darwin.send(fd, p, left, 0)
                #endif
                if n < 0 {
                    if errno == EINTR { continue }
                    return false
                }
                p = p.advanced(by: n)
                left -= n
            }
            return true
        }
    }

    // MARK: utilidades

    /// Lê do socket com um buffer, para separar cabeçalho e corpo.
    final class Connection {
        let fd: Int32
        var buffer = [UInt8]()

        init(fd: Int32) { self.fd = fd }

        private func fill() -> Bool {
            var tmp = [UInt8](repeating: 0, count: 64 * 1024)
            while true {
                let n = tmp.withUnsafeMutableBytes { recv(fd, $0.baseAddress, $0.count, 0) }
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { return false }
                buffer.append(contentsOf: tmp[0..<n])
                return true
            }
        }

        /// Cabeçalho até "\r\n\r\n" (sem ele). nil = conexão fechada ou cabeçalho grande demais.
        func readHead() -> [UInt8]? {
            var searchFrom = 0
            while true {
                if buffer.count >= 4 {
                    var i = max(0, searchFrom - 3)
                    while i + 3 < buffer.count {
                        if buffer[i] == 13 && buffer[i + 1] == 10 && buffer[i + 2] == 13 && buffer[i + 3] == 10 {
                            let head = Array(buffer[0..<i])
                            buffer.removeFirst(i + 4)
                            return head
                        }
                        i += 1
                    }
                    searchFrom = buffer.count
                }
                if buffer.count > HTTPServer.maxHeaderBytes { return nil }
                guard fill() else { return nil }
            }
        }

        func readBody(_ n: Int) -> Data? {
            while buffer.count < n {
                guard fill() else { return nil }
            }
            let body = Data(buffer[0..<n])
            buffer.removeFirst(n)
            return body
        }
    }

    struct Head {
        let method: String
        let target: String
        let version: String
        let headers: [String: String]
    }

    static func parseHead(_ bytes: [UInt8]) -> Head? {
        let text = String(decoding: bytes, as: UTF8.self)
        var lines = text.components(separatedBy: "\r\n")
        // tolera linhas em branco antes da requisição (RFC 9112 §2.2)
        while lines.first == "" { lines.removeFirst() }
        guard let first = lines.first else { return nil }
        let parts = first.split(separator: " ", omittingEmptySubsequences: true)
        guard parts.count == 3, parts[2].hasPrefix("HTTP/1.") else { return nil }
        var headers: [String: String] = [:]
        for line in lines.dropFirst() where !line.isEmpty {
            guard let colon = line.firstIndex(of: ":") else { return nil }
            let name = line[..<colon].trimmingCharacters(in: .whitespaces).lowercased()
            let value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            headers[name] = headers[name].map { "\($0), \(value)" } ?? value
        }
        return Head(method: String(parts[0]).uppercased(), target: String(parts[1]), version: String(parts[2]), headers: headers)
    }

    static func unescape(_ s: String) -> String {
        s.replacingOccurrences(of: "+", with: " ").removingPercentEncoding ?? s
    }

    public static func jsonError(_ status: Int, _ code: String, _ message: String) -> Response {
        let body: [String: Any] = ["error": ["code": code, "message": message]]
        return Response(status: status, headers: [("Content-Type", "application/json; charset=utf-8")],
                        body: .data(JSON.data(body)))
    }

    static func errorResponse(_ status: Int) -> Response {
        var r = jsonError(status, "bad_request", "requisição HTTP inválida")
        r.close = true
        return r
    }

    static func reason(_ status: Int) -> String {
        switch status {
        case 200: return "OK"
        case 204: return "No Content"
        case 206: return "Partial Content"
        case 400: return "Bad Request"
        case 401: return "Unauthorized"
        case 404: return "Not Found"
        case 409: return "Conflict"
        case 412: return "Precondition Failed"
        case 416: return "Range Not Satisfiable"
        case 426: return "Upgrade Required"
        case 429: return "Too Many Requests"
        case 500: return "Internal Server Error"
        default: return "Status"
        }
    }

    // MARK: sockets

    #if os(Linux)
    static let sockStream = Int(SOCK_STREAM.rawValue)
    static let sockDgram = Int(SOCK_DGRAM.rawValue)
    #else
    static let sockStream = Int(SOCK_STREAM)
    static let sockDgram = Int(SOCK_DGRAM)
    #endif

    /// Cria e liga um socket IPv4 em host:port (porta 0 = qualquer livre).
    static func bindSocket(type: Int32, host: String, port: Int) throws -> Int32 {
        let fd = socket(AF_INET, type, 0)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        #if !os(Linux)
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        #endif
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(UInt16(port).bigEndian)
        guard inet_pton(AF_INET, host, &addr.sin_addr) == 1 else {
            close(fd)
            throw POSIXError(.EINVAL)
        }
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard rc == 0 else {
            let e = errno
            close(fd)
            throw POSIXError(POSIXErrorCode(rawValue: e) ?? .EADDRINUSE)
        }
        return fd
    }

    static func boundPort(_ fd: Int32) -> Int {
        var addr = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
        }
        return Int(UInt16(bigEndian: addr.sin_port))
    }

    static func ipString(_ addr: sockaddr_in) -> String {
        var a = addr.sin_addr
        var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &a, &buf, socklen_t(INET_ADDRSTRLEN))
        return String(cString: buf)
    }

    static func setTimeout(_ fd: Int32, _ option: Int32, seconds: Int) {
        var tv = timeval(tv_sec: seconds, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, option, &tv, socklen_t(MemoryLayout<timeval>.size))
    }

    static func configureClientSocket(_ fd: Int32) {
        setTimeout(fd, SO_RCVTIMEO, seconds: idleTimeout)
        setTimeout(fd, SO_SNDTIMEO, seconds: idleTimeout)
        var yes: Int32 = 1
        setsockopt(fd, Int32(IPPROTO_TCP), TCP_NODELAY, &yes, socklen_t(MemoryLayout<Int32>.size))
        var sndbuf = socketSendBuffer
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
        #if !os(Linux)
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
        #endif
    }
}

/// `autoreleasepool` no macOS; no Linux só executa o bloco.
func autoreleasepoolCompat(_ body: () -> Void) {
    #if canImport(ObjectiveC)
    autoreleasepool(invoking: body)
    #else
    body()
    #endif
}
