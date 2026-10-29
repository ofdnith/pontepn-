import XCTest
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
@testable import PonteCore
@testable import PonteServer

final class ServerTests: XCTestCase {
    func testParseRangeRules() throws {
        XCTAssertTrue(try PonteServer.parseRange("bytes=0-", size: 10) == (0, 9))
        XCTAssertTrue(try PonteServer.parseRange("bytes=5-100", size: 10) == (5, 9))
        XCTAssertTrue(try PonteServer.parseRange("bytes=-3", size: 10) == (7, 9))
        for bad in ["bytes=10-", "bytes=5-2", "bytes=-", "items=0-1", "bytes=0-1,3-4"] {
            XCTAssertThrowsError(try PonteServer.parseRange(bad, size: 10), bad)
        }
    }

    func testDiscoveryQuery() {
        XCTAssertTrue(DiscoveryResponder.isQuery(Array("PONTE?1".utf8)))
        XCTAssertTrue(DiscoveryResponder.isQuery(Array(" PONTE?1\n".utf8)))
        XCTAssertFalse(DiscoveryResponder.isQuery(Array("PONTE?2".utf8)))
    }

    func testHTTPHeadParsing() {
        let head = HTTPServer.parseHead(Array("GET /api/v1/catalog?platform=ps5&x=a%20b HTTP/1.1\r\nHost: x\r\nX-Ponte-Protocol: 1".utf8))!
        XCTAssertEqual(head.method, "GET")
        XCTAssertEqual(head.headers["x-ponte-protocol"], "1")
        let req = HTTPServer.Request(method: head.method, target: head.target, headers: head.headers, body: Data(),
                                     remoteIP: "1.2.3.4", bodyTooLarge: false)
        XCTAssertEqual(req.path, "/api/v1/catalog")
        XCTAssertEqual(req.query, ["platform": "ps5", "x": "a b"])
        XCTAssertNil(HTTPServer.parseHead(Array("lixo".utf8)))
    }

    func testSameSecretAndCodes() {
        XCTAssertTrue(PonteServer.sameSecret("123456", "123456"))
        XCTAssertFalse(PonteServer.sameSecret("123456", "123457"))
        XCTAssertFalse(PonteServer.sameSecret("12345", "123456"))
        for _ in 0..<50 { XCTAssertTrue(RX(#"^\d{6}$"#).matches(PonteServer.newCode())) }
        XCTAssertTrue(RX("^[0-9a-f]{64}$").matches(PonteServer.randomHex(32)))
    }

    func testNamesAndPaths() {
        XCTAssertEqual(Preparer.baseName("Jogo.part1.rar"), "Jogo.part1")
        XCTAssertEqual(Preparer.baseName("Jogo.rar"), "Jogo")
        XCTAssertEqual(Preparer.baseName("Jogo.TAR.GZ"), "Jogo")
        XCTAssertEqual(Preparer.volumeLabel("PPSA01234"), "PPSA01234")
        XCTAssertEqual(Preparer.volumeLabel("já é/um nome muito comprido"), "JUMNOMEMUITOCOM")
        XCTAssertEqual(Preparer.safeFileName("a/b:c"), "a_b_c")
        let dir = tempDir()
        let p = joinPath(dir, "x.exfat")
        XCTAssertEqual(Preparer.uniquePath(p), p)
        FileManager.default.createFile(atPath: p, contents: Data())
        XCTAssertEqual(Preparer.uniquePath(p), joinPath(dir, "x (2).exfat"))
    }

    // MARK: preparação

    func tempDir() -> String {
        let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("ponte-srv-\(UUID().uuidString)")
        try! FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    func makeGame(_ dir: String) {
        let root = joinPath(dir, "PPSA01234")
        try! FileManager.default.createDirectory(atPath: joinPath(root, "sce_sys"), withIntermediateDirectories: true)
        try! Data(#"{"titleId":"PPSA01234","localizedParameters":{"defaultLanguage":"en","en":{"titleName":"Jogo"}}}"#.utf8)
            .write(to: URL(fileURLWithPath: joinPath(root, "sce_sys/param.json")))
        try! Data((0..<300_000).map { _ in UInt8.random(in: 0...255) }).write(to: URL(fileURLWithPath: joinPath(root, "eboot.bin")))
    }

    func runTool(_ tool: String, _ args: [String], cwd: String) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        p.currentDirectoryURL = URL(fileURLWithPath: cwd)
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        try! p.run()
        p.waitUntilExit()
        return p.terminationStatus
    }

    func waitIdle(_ prep: Preparer, timeout: Double = 20) {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while prep.current != nil && Date() < deadline { usleep(50_000) }
        XCTAssertNil(prep.current)
    }

    func testExtractSplitArchiveWithPassword() throws {
        guard let sz = Preparer.findTool(["7zz", "7z"]) else { throw XCTSkip("7-Zip não instalado") }
        let lib = tempDir()
        let src = tempDir()
        makeGame(src)
        // 7z dividido em volumes de 100 KB, com senha
        XCTAssertEqual(runTool(sz, ["a", "-pSegredo1", "-mhe=on", "-v100k", joinPath(lib, "Jogo.7z"), "PPSA01234"], cwd: src), 0)
        let parts = try FileManager.default.contentsOfDirectory(atPath: lib).sorted()
        XCTAssertGreaterThan(parts.count, 1)

        let library = Library(root: lib, stableSeconds: 0)
        let prep = Preparer(library: library, log: { _ in })
        prep.extractArchives = true
        library.scan()
        XCTAssertEqual(library.archives.count, 1)
        XCTAssertTrue(library.archives[0].complete)

        prep.tick()
        waitIdle(prep)
        let problem = try XCTUnwrap(prep.problems.first)
        XCTAssertTrue(problem.needsPassword, problem.reason)
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(lib, "Jogo.ponte-tmp")))

        prep.setPassword("Segredo1", for: problem.key)
        library.scan()
        prep.tick()
        waitIdle(prep)
        XCTAssertEqual(prep.problems, [])
        library.scan()
        XCTAssertEqual(library.items().map(\.rel), ["PPSA01234"])  // achatado na raiz
        XCTAssertEqual(library.archives, [])
        let kept = try FileManager.default.contentsOfDirectory(atPath: joinPath(lib, "_originais")).sorted()
        XCTAssertEqual(kept, parts)
    }

    func testExtractZipWithSeveralTopLevelEntries() throws {
        guard let zip = Preparer.findTool(["zip"]), Preparer.findTool(["7zz", "7z", "bsdtar"]) != nil else {
            throw XCTSkip("zip/extrator não instalados")
        }
        let lib = tempDir()
        let src = tempDir()
        makeGame(src)
        try Data("oi".utf8).write(to: URL(fileURLWithPath: joinPath(src, "leia.txt")))
        XCTAssertEqual(runTool(zip, ["-qr", joinPath(lib, "Pacote.zip"), "PPSA01234", "leia.txt"], cwd: src), 0)
        let library = Library(root: lib, stableSeconds: 0)
        let prep = Preparer(library: library, log: { _ in })
        prep.extractArchives = true
        prep.keepOriginals = false
        library.scan()
        prep.tick()
        waitIdle(prep)
        XCTAssertEqual(prep.problems, [])
        library.scan()
        // dois itens na raiz do zip: vão para a pasta "Pacote", ainda dentro do limite de profundidade
        XCTAssertEqual(library.items().map(\.rel), ["Pacote/PPSA01234"])
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(lib, "Pacote.zip")))
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(lib, "_originais")))
    }

    func testIncompleteSetIsReported() {
        let lib = tempDir()
        for n in ["Jogo.7z.001", "Jogo.7z.003"] { FileManager.default.createFile(atPath: joinPath(lib, n), contents: Data([1])) }
        let library = Library(root: lib, stableSeconds: 0)
        let prep = Preparer(library: library, log: { _ in })
        prep.extractArchives = true
        library.scan()
        prep.tick()
        XCTAssertNil(prep.current)
        XCTAssertEqual(prep.problems.map(\.reason), ["conjunto incompleto: faltam as partes 2"])
        FileManager.default.createFile(atPath: joinPath(lib, "Jogo.7z.002"), contents: Data([1]))
        library.scan()
        prep.tick()
        waitIdle(prep)
        // agora completo: tenta extrair (e falha, porque o conteúdo é lixo), mas não é mais "incompleto"
        XCTAssertFalse(prep.problems.contains { $0.reason.hasPrefix("conjunto incompleto") })
    }

    func testFoldersAreHeldForConversion() {
        let lib = tempDir()
        makeGame(lib)
        let library = Library(root: lib, stableSeconds: 0)
        library.holdFolders = true
        library.scan()
        XCTAssertEqual(library.items().count, 0)
        XCTAssertEqual(library.toConvert.map { lastComponent($0.path) }, ["PPSA01234"])
        XCTAssertTrue(library.pendingIds().contains(Library.itemId(platform: "ps5", rel: "PPSA01234")))
        library.holdFolders = false
        library.scan()
        XCTAssertEqual(library.items().count, 1)
    }

    func testConversionNeedsMacOSTools() {
        let lib = tempDir()
        makeGame(lib)
        let library = Library(root: lib, stableSeconds: 0)
        library.holdFolders = true
        let prep = Preparer(library: library, log: { _ in })
        prep.convertFolders = true
        prep.tools.hdiutil = "/nao/existe/hdiutil"
        library.scan()
        prep.tick()
        waitIdle(prep)
        XCTAssertEqual(prep.problems.map(\.reason), ["a conversão para .exfat só funciona no macOS"])
    }

    /// Fluxo completo da conversão com ferramentas falsas (no Mac de verdade são mkfile, hdiutil,
    /// newfs_exfat e diskutil). Confere a ordem dos comandos, a cópia e o resultado na biblioteca.
    func testConversionFlowWithFakeTools() throws {
        let lib = tempDir()
        let bin = tempDir()
        let capture = tempDir()
        let logFile = joinPath(bin, "log.txt")
        makeGame(lib)
        try Data([0x89, 0x50, 0x4E, 0x47]).write(to: URL(fileURLWithPath: joinPath(lib, "PPSA01234/sce_sys/icon0.png")))
        try Data("finder".utf8).write(to: URL(fileURLWithPath: joinPath(lib, "PPSA01234/.DS_Store")))
        func script(_ name: String, _ body: String) -> String {
            let path = joinPath(bin, name)
            try! "#!/bin/sh\necho \"\(name) $*\" >> '\(logFile)'\n\(body)\n".write(toFile: path, atomically: true, encoding: .utf8)
            chmod(path, 0o755)
            return path
        }
        let tools = Preparer.Tools(
            mkfile: script("mkfile", #"n=${2%m}; truncate -s "${n}M" "$3""#),
            hdiutil: script("hdiutil", #"[ "$1" = attach ] && printf '/dev/disk9          \t\n'; exit 0"#),
            newfsExfat: script("newfs_exfat", "exit 0"),
            diskutil: script("diskutil", "[ \"$1\" = unmount ] && cp -R \"$2/.\" '\(capture)/'; exit 0"),
            mount: script("mount", "exit 1"),
            umount: script("umount", "exit 0"))

        let library = Library(root: lib, stableSeconds: 0)
        library.holdFolders = true
        let prep = Preparer(library: library, log: { _ in })
        prep.convertFolders = true
        prep.tools = tools
        library.scan()
        prep.tick()
        XCTAssertEqual(prep.current?.stage, .converting)
        waitIdle(prep)
        XCTAssertEqual(prep.problems, [])

        let log = try String(contentsOfFile: logFile, encoding: .utf8).split(separator: "\n").map { String($0.split(separator: " ").first!) }
        XCTAssertEqual(log, ["mkfile", "hdiutil", "newfs_exfat", "diskutil", "diskutil", "hdiutil"])
        let logText = try String(contentsOfFile: logFile, encoding: .utf8)
        XCTAssertTrue(logText.contains("hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount"), logText)
        XCTAssertTrue(logText.contains("newfs_exfat -b 32768 -v PPSA01234 /dev/disk9"), logText)
        XCTAssertTrue(logText.contains("hdiutil detach /dev/disk9"), logText)

        // conteúdo copiado para dentro da imagem, sem o lixo do Finder
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: joinPath(capture, "eboot.bin"))),
                       try Data(contentsOf: URL(fileURLWithPath: joinPath(lib, "_originais/PPSA01234/eboot.bin"))))
        XCTAssertTrue(isFile(joinPath(capture, "sce_sys/param.json")))
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(capture, ".DS_Store")))

        // na biblioteca: a imagem com arquivo lateral e ícone; a pasta foi para _originais
        XCTAssertTrue(isFile(joinPath(lib, "PPSA01234.exfat")))
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(lib, "PPSA01234.exfat.ponte-tmp")))
        XCTAssertTrue(isFile(joinPath(lib, "PPSA01234.exfat.ponte.png")))
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(lib, "PPSA01234")))
        library.scan()
        let item = try XCTUnwrap(library.items().first)
        XCTAssertEqual(item.ident.kind, "exfat")
        XCTAssertEqual(item.title, "Jogo")
        XCTAssertEqual(item.allWarnings, [])
        XCTAssertTrue(item.hasIcon)
        let plan = try ExfatPlan.make(folder: joinPath(lib, "_originais/PPSA01234"))
        XCTAssertEqual(fileStat(joinPath(lib, "PPSA01234.exfat"))?.size, plan.sizeBytes)
    }

    func testConversionFailureCleansUp() throws {
        let lib = tempDir()
        let bin = tempDir()
        makeGame(lib)
        func script(_ name: String, _ body: String) -> String {
            let path = joinPath(bin, name)
            try! "#!/bin/sh\n\(body)\n".write(toFile: path, atomically: true, encoding: .utf8)
            chmod(path, 0o755)
            return path
        }
        let library = Library(root: lib, stableSeconds: 0)
        library.holdFolders = true
        let prep = Preparer(library: library, log: { _ in })
        prep.convertFolders = true
        prep.tools = Preparer.Tools(
            mkfile: script("mkfile", #"n=${2%m}; truncate -s "${n}M" "$3""#),
            hdiutil: script("hdiutil", #"[ "$1" = attach ] && echo /dev/disk9; exit 0"#),
            newfsExfat: script("newfs_exfat", "echo 'deu ruim' >&2; exit 1"),
            diskutil: script("diskutil", "exit 0"), mount: script("mount", "exit 0"), umount: script("umount", "exit 0"))
        library.scan()
        prep.tick()
        waitIdle(prep)
        XCTAssertEqual(prep.problems.count, 1)
        XCTAssertTrue(prep.problems[0].reason.contains("newfs_exfat falhou: deu ruim"), prep.problems[0].reason)
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(lib, "PPSA01234.exfat.ponte-tmp")))
        XCTAssertTrue(isDirectory(joinPath(lib, "PPSA01234")))  // a pasta fica
        // não tenta de novo sozinho; "tentar de novo" limpa o problema
        library.scan()
        prep.tick()
        XCTAssertNil(prep.current)
        prep.retry(prep.problems[0].key)
        XCTAssertEqual(prep.problems, [])
    }

    // MARK: cópias em andamento e parada para ejetar

    final class Box: @unchecked Sendable {
        var status = 0
        var data = Data()
    }

    func request(_ url: String, method: String = "GET", json: String? = nil) -> (Int, Data) {
        var req = URLRequest(url: URL(string: url)!)
        req.httpMethod = method
        if let json {
            req.httpBody = Data(json.utf8)
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        let box = Box()
        let sem = DispatchSemaphore(value: 0)
        URLSession.shared.dataTask(with: req) { data, resp, _ in
            box.status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            box.data = data ?? Data()
            sem.signal()
        }.resume()
        _ = sem.wait(timeout: .now() + 20)
        return (box.status, box.data)
    }

    func testActiveTransfersAndStopAndWait() throws {
        let lib = tempDir()
        makeGame(lib)
        var config = PonteConfig(library: lib, profilesDir: tempDir(), stateDir: tempDir(), name: "teste")
        config.port = 0
        config.host = "127.0.0.1"
        config.discoveryPort = nil
        config.pairing = false
        config.stableSeconds = 0
        let server = try PonteServer(config: config, log: { _ in })
        try server.start()
        let item = try XCTUnwrap(server.library.items().first)
        let base = "http://127.0.0.1:\(server.port)/api/v1/items/\(item.id)"
        XCTAssertEqual(server.activeTransfers(), [])

        // baixar um arquivo marca o item como em cópia (inclusive logo depois de terminar o arquivo)
        let (status, data) = request(base + "/files/0")
        XCTAssertEqual(status, 200)
        XCTAssertEqual(data.count, 300_000)
        XCTAssertEqual(server.activeTransfers(), [ActiveTransfer(itemId: item.id, title: item.title)])
        // o console já recebeu tudo; o servidor fecha o envio um instante depois
        waitUntil { server.currentTransfers.first?.streaming == false }
        // sem folga: nada sendo enviado neste instante
        XCTAssertEqual(server.activeTransfers(grace: -1), [])

        // detalhes da cópia em andamento
        let snap = try XCTUnwrap(server.currentTransfers.first)
        XCTAssertEqual(snap.itemId, item.id)
        XCTAssertEqual(snap.client, "127.0.0.1")
        XCTAssertEqual(snap.totalBytes, item.size)
        XCTAssertEqual(snap.doneBytes, 300_000)
        XCTAssertEqual(snap.sentBytes, 300_000)
        XCTAssertEqual(snap.fileCount, 2)
        XCTAssertEqual(snap.currentFile, "eboot.bin")
        XCTAssertNil(snap.reportedResult)

        // o report diz que a cópia acabou: sai das cópias em andamento e vai para o histórico
        XCTAssertEqual(request(base + "/report", method: "POST", json: #"{"result":"copied","storage":"ext0","duration_s":3}"#).0, 204)
        XCTAssertEqual(server.activeTransfers(), [])
        XCTAssertEqual(server.currentTransfers, [])
        let rec = try XCTUnwrap(server.history.first)
        XCTAssertEqual(rec.result, "copied")
        XCTAssertEqual(rec.storage, "ext0")
        XCTAssertEqual(rec.durationS, 3)
        XCTAssertEqual(rec.sentBytes, 300_000)
        XCTAssertEqual(rec.totalBytes, item.size)
        XCTAssertEqual(rec.client, "127.0.0.1")

        // parar e esperar: conexões fechadas, threads paradas
        XCTAssertTrue(server.stopAndWait(timeout: 10))
        XCTAssertEqual(server.activeTransfers(), [])
    }

    func testCancelledConversionIsNotAProblem() throws {
        let lib = tempDir()
        let bin = tempDir()
        makeGame(lib)
        func script(_ name: String, _ body: String) -> String {
            let path = joinPath(bin, name)
            try! "#!/bin/sh\n\(body)\n".write(toFile: path, atomically: true, encoding: .utf8)
            chmod(path, 0o755)
            return path
        }
        let library = Library(root: lib, stableSeconds: 0)
        library.holdFolders = true
        let prep = Preparer(library: library, log: { _ in })
        prep.convertFolders = true
        // mkfile demora: dá tempo de cancelar no meio, como ao ejetar o disco
        prep.tools = Preparer.Tools(
            mkfile: script("mkfile", "exec sleep 30"),
            hdiutil: script("hdiutil", "exit 0"), newfsExfat: script("newfs_exfat", "exit 0"),
            diskutil: script("diskutil", "exit 0"), mount: script("mount", "exit 0"), umount: script("umount", "exit 0"))
        library.scan()
        prep.tick()
        XCTAssertEqual(prep.current?.stage, .converting)
        usleep(300_000)
        prep.cancel()
        XCTAssertTrue(prep.waitUntilIdle(timeout: 10))
        XCTAssertEqual(prep.problems, [])  // interrompida não é falha
        XCTAssertFalse(FileManager.default.fileExists(atPath: joinPath(lib, "PPSA01234.exfat.ponte-tmp")))
        XCTAssertTrue(isDirectory(joinPath(lib, "PPSA01234")))  // a pasta fica para recomeçar depois
        // cancelado não começa trabalho novo até `resume()`
        library.scan()
        prep.tick()
        XCTAssertNil(prep.current)
        prep.resume()
        prep.tick()
        XCTAssertEqual(prep.current?.stage, .converting)
        prep.cancel()
        XCTAssertTrue(prep.waitUntilIdle(timeout: 10))
    }

    /// Com um console copiando, a conversão cede a vez (aparece pausada) e continua sozinha depois.
    func testPreparerYieldsWhileCopying() throws {
        let lib = tempDir()
        let bin = tempDir()
        makeGame(lib)
        func script(_ name: String, _ body: String) -> String {
            let path = joinPath(bin, name)
            try! "#!/bin/sh\n\(body)\n".write(toFile: path, atomically: true, encoding: .utf8)
            chmod(path, 0o755)
            return path
        }
        let library = Library(root: lib, stableSeconds: 0)
        library.holdFolders = true
        let prep = Preparer(library: library, log: { _ in })
        prep.convertFolders = true
        prep.tools = Preparer.Tools(
            mkfile: script("mkfile", "sleep 0.3; exit 1"),
            hdiutil: script("hdiutil", "exit 0"), newfsExfat: script("newfs_exfat", "exit 0"),
            diskutil: script("diskutil", "exit 0"), mount: script("mount", "exit 0"), umount: script("umount", "exit 0"))
        final class Flag: @unchecked Sendable { var on = true }
        let copying = Flag()
        prep.shouldYield = { copying.on }
        library.scan()
        prep.tick()
        waitUntil { prep.current?.paused == true }
        usleep(800_000)
        XCTAssertNotNil(prep.current)  // esperando: com o console copiando, o mkfile nem começa
        copying.on = false
        waitIdle(prep)
        // mkfile falso falha de propósito: o trabalho terminou (virou problema), não ficou preso
        XCTAssertEqual(prep.problems.count, 1)
    }

    // MARK: progresso e histórico das cópias

    func waitUntil(_ timeout: Double = 5, _ cond: () -> Bool) {
        let deadline = Date(timeIntervalSinceNow: timeout)
        while !cond() && Date() < deadline { usleep(20_000) }
        XCTAssertTrue(cond())
    }

    final class Clock: @unchecked Sendable {
        var t = Date(timeIntervalSince1970: 1_800_000_000)
        func advance(_ s: Double) { t = t.addingTimeInterval(s) }
    }

    func fakeItem(kind: String, sizes: [Int64]) -> Item {
        let files = sizes.enumerated().map { i, n in
            FileEntry(index: i, path: "f\(i).bin", absPath: "/nao/existe/f\(i).bin", size: n, mtimeNs: 0)
        }
        let ident = Identified(platform: "ps5", kind: kind, content: "base", title: "Jogo Teste", titleId: "PPSA01234")
        return Item(id: "0123456789abcdef", revision: "r1", rel: "x", absPath: "/nao/existe", ident: ident, files: files, addedTS: 0)
    }

    func testResumeCountsWhatTheConsoleAlreadyHadAndSpeed() {
        let clock = Clock()
        let tracker = TransferTracker(historyFile: nil, now: { clock.t })
        let item = fakeItem(kind: "exfat", sizes: [1_000_000, 3_000_000])
        let key = TransferTracker.liveKey(itemId: item.id, client: "tok")
        // o console já tinha 400 KB do primeiro arquivo: pede Range a partir dali
        let a = TransferStream(tracker: tracker, key: key, item: item, client: "PS5 da sala", index: 0, offset: 400_000)
        a.start()
        for _ in 0..<6 {
            clock.advance(1)
            a.add(100_000)
        }
        a.end()
        let b = TransferStream(tracker: tracker, key: key, item: item, client: "PS5 da sala", index: 1, offset: 0)
        b.start()
        for _ in 0..<5 {
            clock.advance(1)
            b.add(500_000)
        }
        let s = tracker.snapshot()[0]
        XCTAssertEqual(s.client, "PS5 da sala")
        XCTAssertEqual(s.doneBytes, 1_000_000 + 2_500_000)
        XCTAssertEqual(s.sentBytes, 600_000 + 2_500_000)
        XCTAssertEqual(s.totalBytes, 4_000_000)
        XCTAssertEqual(s.fraction, 0.875, accuracy: 0.0001)
        XCTAssertEqual(s.currentFile, "f1.bin")
        XCTAssertEqual(s.currentFileIndex, 1)
        XCTAssertTrue(s.streaming)
        XCTAssertEqual(s.bytesPerSecond, 500_000, accuracy: 1)  // últimos 5 s
        XCTAssertEqual(s.secondsLeft ?? -1, 1, accuracy: 0.01)
        b.end()
        XCTAssertEqual(tracker.snapshot()[0].bytesPerSecond, 0)  // parado não tem velocidade
    }

    func testStalledCopyBecomesInterruptedAndHistoryPersists() throws {
        let clock = Clock()
        let file = joinPath(tempDir(), "history.json")
        let tracker = TransferTracker(historyFile: file, now: { clock.t })
        let item = fakeItem(kind: "exfat", sizes: [1_000_000])
        let key = TransferTracker.liveKey(itemId: item.id, client: "ip")
        let st = TransferStream(tracker: tracker, key: key, item: item, client: "Switch (10.0.0.9)", index: 0, offset: 0)
        st.start()
        clock.advance(2)
        st.add(250_000)
        st.end()
        clock.advance(60)
        XCTAssertEqual(tracker.snapshot().count, 1)  // pausa curta: ainda em andamento
        clock.advance(61)
        XCTAssertEqual(tracker.snapshot(), [])  // 2 min parado sem resultado
        let rec = try XCTUnwrap(tracker.records().first)
        XCTAssertEqual(rec.result, "interrupted")
        XCTAssertEqual(rec.sentBytes, 250_000)
        XCTAssertEqual(rec.averageBytesPerSecond ?? 0, 125_000, accuracy: 1)

        // um resultado sem nada baixado (falha antes de começar) também entra
        tracker.report(item: item, client: "Switch (10.0.0.9)", key: key, result: "failed", storage: nil,
                       detail: "sem espaço", durationS: 0)
        XCTAssertEqual(tracker.records().map(\.result), ["failed", "interrupted"])

        // guardado entre execuções
        let again = TransferTracker(historyFile: file, now: { clock.t })
        XCTAssertEqual(again.records(), tracker.records())
        again.clearHistory()
        XCTAssertEqual(TransferTracker(historyFile: file).records(), [])
    }

    func testPkgInstallerDownloadCompletesTheInstalledRecord() throws {
        let clock = Clock()
        let tracker = TransferTracker(historyFile: nil, now: { clock.t })
        let item = fakeItem(kind: "pkg_ps4", sizes: [2_000_000])
        let key = TransferTracker.liveKey(itemId: item.id, client: "tok")
        // o payload entrega a URL ao instalador e já diz "installed"
        tracker.report(item: item, client: "PS5", key: key, result: "installed", storage: "ext0", detail: nil, durationS: 1)
        XCTAssertEqual(tracker.records().count, 1)
        // o instalador baixa depois, em partes: aparece como em andamento, já com o resultado
        clock.advance(5)
        let a = TransferStream(tracker: tracker, key: key, item: item, client: "PS5", index: 0, offset: 0)
        a.start()
        clock.advance(1)
        a.add(1_000_000)
        a.end()
        XCTAssertEqual(tracker.snapshot().first?.reportedResult, "installed")
        clock.advance(10)
        let b = TransferStream(tracker: tracker, key: key, item: item, client: "PS5", index: 0, offset: 1_000_000)
        b.start()
        clock.advance(1)
        b.add(1_000_000)
        b.end()
        XCTAssertEqual(tracker.snapshot().first?.doneBytes, 2_000_000)
        clock.advance(31)
        XCTAssertEqual(tracker.snapshot(), [])
        // um registro só, completado com o que o Mac enviou
        let recs = tracker.records()
        XCTAssertEqual(recs.count, 1)
        XCTAssertEqual(recs[0].result, "installed")
        XCTAssertEqual(recs[0].sentBytes, 2_000_000)
        XCTAssertNotNil(recs[0].startedAt)
    }
}
