import Foundation
import XCTest
@testable import PonteCore

/// Dados sintéticos, os mesmos da referência em Python (reference/tests/conftest.py).
enum Fx {
    static let png: [UInt8] = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A] + [UInt8](repeating: 0, count: 24)

    static var repoRoot: String {
        // .../mac/Tests/PonteCoreTests/Fixtures.swift -> raiz do repositório
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<4 { url.deleteLastPathComponent() }
        return url.path
    }

    static func tempDir(_ name: String = "t") -> String {
        let dir = (NSTemporaryDirectory() as NSString).appendingPathComponent("ponte-\(name)-\(UUID().uuidString)")
        try! FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    @discardableResult
    static func write(_ path: String, _ bytes: [UInt8]) -> String {
        try! FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                                 withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: path, contents: Data(bytes))
        return path
    }

    @discardableResult
    static func write(_ path: String, _ text: String) -> String { write(path, Array(text.utf8)) }

    @discardableResult
    static func ps5Folder(_ parent: String, name: String = "PPSA01234", title: String = "Jogo Cinco",
                          eboot: Bool = true, extra: [String: [UInt8]] = [:]) -> String {
        let root = joinPath(parent, name)
        write(joinPath(root, "sce_sys/param.json"), """
        {"titleId": "PPSA01234", "contentId": "UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP",
         "contentVersion": "01.000.000", "requiredSystemSoftwareVersion": "0x0403000000000000",
         "localizedParameters": {"defaultLanguage": "pt-BR", "pt-BR": {"titleName": "\(title)"}}}
        """)
        write(joinPath(root, "sce_sys/icon0.png"), png)
        if eboot { write(joinPath(root, "eboot.bin"), [UInt8](repeating: 0x45, count: 5000)) }
        write(joinPath(root, "data/a.bin"), [UInt8](repeating: 0x41, count: 1234))
        for (rel, content) in extra { write(joinPath(root, rel), content) }
        return root
    }

    static func sfo(category: String = "gd", title: String = "Jogo Quatro", titleId: String = "CUSA00001",
                    appVer: String = "01.00", version: String = "01.00", systemVer: UInt32 = 0x05050000) -> [UInt8] {
        SFO.build(["CATEGORY": .string(category), "TITLE": .string(title), "TITLE_ID": .string(titleId),
                   "APP_VER": .string(appVer), "VERSION": .string(version), "SYSTEM_VER": .int(systemVer),
                   "CONTENT_ID": .string("UP0000-\(titleId)_00-ABCDEFGHIJKLMNOP")])
    }

    @discardableResult
    static func ps4Pkg(_ path: String, titleId: String = "CUSA00001", category: String = "gd",
                       appVer: String = "01.00", version: String = "01.00", systemVer: UInt32 = 0x05050000,
                       icon: Bool = true) -> String {
        let s = sfo(category: category, titleId: titleId, appVer: appVer, version: version, systemVer: systemVer)
        return write(path, PKG.build(contentId: "UP0000-\(titleId)_00-ABCDEFGHIJKLMNOP", sfo: s,
                                     icon: icon ? png : nil, body: [UInt8](repeating: 0x42, count: 4096)))
    }

    @discardableResult
    static func nsp(_ path: String, tid: String = "0100ABCD12340000", ticket: Bool = true) -> String {
        var files: [(String, [UInt8])] = [("abcdef0123456789abcdef0123456789.nca", [UInt8](repeating: 0x4E, count: 3000))]
        if ticket { files.append(("\(tid.lowercased())0000000000000000.tik", [UInt8](repeating: 0x54, count: 0x2C0))) }
        return write(path, PFS0.build(files))
    }

    /// Deixa o arquivo/pasta "velho", para passar na regra de estabilidade.
    static func age(_ path: String, seconds: Double = 3600) {
        let date = Date(timeIntervalSinceNow: -seconds)
        var targets = [path]
        if isDirectory(path), let walker = FileManager.default.enumerator(atPath: path) {
            targets = []
            while let rel = walker.nextObject() as? String {
                let p = joinPath(path, rel)
                if isFile(p) { targets.append(p) }
            }
        }
        for p in targets { try? FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: p) }
    }
}

extension Identification {
    var item: Identified {
        guard case .identified(let i) = self else {
            XCTFail("esperava item identificado, veio \(self)")
            return Identified(platform: "", kind: "")
        }
        return i
    }

    var isAttention: Bool {
        if case .attention = self { return true }
        return false
    }
}
