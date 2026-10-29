import XCTest
@testable import PonteCore

final class LibraryTests: XCTestCase {
    var root = ""

    override func setUp() { root = Fx.tempDir("lib") }
    override func tearDown() { try? FileManager.default.removeItem(atPath: root) }

    var profiles: [Profile] { Profiles.load(directory: joinPath(Fx.repoRoot, "profiles")) }

    func buildLibrary() {
        Fx.ps5Folder(root)
        Fx.nsp(joinPath(root, "Switch/Jogo [0100ABCD12340000][v0].nsp"))
        Fx.nsp(joinPath(root, "Switch/Jogo [0100ABCD12340800][v65536].nsp"), tid: "0100ABCD12340800")
        Fx.ps4Pkg(joinPath(root, "velho.pkg"))
        Fx.write(joinPath(root, "PPSA09999.exfat"), [UInt8](repeating: 0, count: 64))
        Fx.write(joinPath(root, "Jogo.part1.rar"), "r")
        Fx.write(joinPath(root, "Jogo.part2.rar"), "r")
        Fx.write(joinPath(root, "leia.txt"), "oi")
        Fx.age(root)
    }

    func testScanFindsEverything() {
        buildLibrary()
        let lib = Library(root: root)
        lib.scan()
        let kinds = lib.items().map { "\($0.platform):\($0.ident.kind)" }.sorted()
        XCTAssertEqual(kinds, ["ps5:exfat", "ps5:folder", "ps5:pkg_ps4", "switch:nsp", "switch:nsp"])
        XCTAssertEqual(lib.archives.map(\.key), ["Jogo.rar"])
        XCTAssertEqual(lib.attention, [])
        XCTAssertEqual(lib.pending, [])
    }

    func testIdsAreStableAndRevisionTracksContent() {
        buildLibrary()
        let lib = Library(root: root)
        lib.scan()
        let folder = lib.items().first { $0.ident.kind == "folder" }!
        XCTAssertEqual(folder.id, Library.itemId(platform: "ps5", rel: "PPSA01234"))
        let rev = folder.revision
        lib.scan()
        XCTAssertEqual(lib.get(folder.id)?.revision, rev)
        Fx.write(joinPath(root, "PPSA01234/eboot.bin"), [UInt8](repeating: 0x58, count: 10))
        Fx.age(root)
        lib.scan()
        XCTAssertNotEqual(lib.get(folder.id)?.revision, rev)
    }

    func testFolderFilesSortedRelativeAndWithoutFinderJunk() {
        Fx.ps5Folder(root, extra: ["Z/z.bin": [1], "B.bin": [2], ".DS_Store": [3], "data/._a.bin": [4], "a.bin": [5]])
        Fx.age(root)
        let lib = Library(root: root)
        lib.scan()
        let item = lib.items()[0]
        let files = (lib.detail(item)["files"] as! [[String: Any]])
        let paths = files.map { $0["path"] as! String }
        XCTAssertEqual(paths, ["B.bin", "Z/z.bin", "a.bin", "data/a.bin", "eboot.bin", "sce_sys/icon0.png", "sce_sys/param.json"])
        XCTAssertEqual(files.map { $0["index"] as! Int }, Array(0..<paths.count))
        XCTAssertEqual(item.summary()["file_count"] as? Int, 7)
    }

    func testUnstableAndPartialDownloadsWait() {
        Fx.ps5Folder(root)  // recém-criada: instável
        let nsp = Fx.nsp(joinPath(root, "A [0100ABCD12340000].nsp"))
        Fx.age(nsp)
        Fx.write(joinPath(root, "A [0100ABCD12340000].nsp.crdownload"), [])
        let lib = Library(root: root, stableSeconds: 10)
        lib.scan()
        XCTAssertTrue(lib.items().isEmpty)
        XCTAssertEqual(lib.pending.count, 2)
        XCTAssertEqual(lib.pendingIds().count, 2)
    }

    func testAttentionForBadItems() {
        Fx.ps5Folder(root, eboot: false)
        Fx.write(joinPath(root, "ruim.pkg"), [UInt8](repeating: 0x78, count: 200))
        Fx.age(root)
        let lib = Library(root: root)
        lib.scan()
        XCTAssertTrue(lib.items().isEmpty)
        XCTAssertEqual(lib.attention.map { lastComponent($0.path) }.sorted(), ["PPSA01234", "ruim.pkg"])
    }

    func testDepthLimitAndGameFolderNotDescended() {
        Fx.nsp(joinPath(root, "a/b/Fundo [0100ABCD12340000].nsp"))
        let game = Fx.ps5Folder(joinPath(root, "a"))
        Fx.nsp(joinPath(game, "dentro [0100ABCD99990000].nsp"))
        Fx.write(joinPath(root, "_originais/PPSA0.exfat"), [0])
        Fx.age(root)
        let lib = Library(root: root)
        lib.scan()
        XCTAssertEqual(lib.items().map(\.rel), ["a/PPSA01234"])
    }

    func testDuplicatesMarkedOnNewest() {
        let a = Fx.nsp(joinPath(root, "A [0100ABCD12340000][v0].nsp"))
        let b = Fx.nsp(joinPath(root, "B [0100ABCD12340000][v0].nsp"))
        Fx.age(a, seconds: 7200)
        Fx.age(b, seconds: 3600)
        let lib = Library(root: root)
        lib.scan()
        let marks = Dictionary(uniqueKeysWithValues: lib.items().map { (lastComponent($0.absPath), $0.allWarnings) })
        XCTAssertFalse(marks[lastComponent(a)]!.contains("duplicate_title_id"))
        XCTAssertTrue(marks[lastComponent(b)]!.contains("duplicate_title_id"))
    }

    func testHashesAndPersistentCache() throws {
        let pkg = Fx.ps4Pkg(joinPath(root, "p.pkg"))
        Fx.age(root)
        let cacheFile = joinPath(Fx.tempDir("cache"), "hashes.json")
        let lib = Library(root: root, hashCacheFile: cacheFile)
        lib.scan()
        let f = lib.items()[0].files[0]
        XCTAssertNil(lib.sha256(f))
        XCTAssertEqual(lib.hashPending(), 1)
        let expected = Library.hashFile(pkg)!
        XCTAssertEqual(lib.sha256(f), expected)
        XCTAssertEqual(expected.count, 64)
        lib.scan()
        XCTAssertEqual(lib.sha256(lib.items()[0].files[0]), expected)
        // outra instância lê o cache do disco
        let again = Library(root: root, hashCacheFile: cacheFile)
        again.scan()
        XCTAssertEqual(again.sha256(again.items()[0].files[0]), expected)
        XCTAssertEqual(again.hashPending(), 0)
    }

    func testGeneratedAtChangesOnlyOnChange() {
        Fx.ps4Pkg(joinPath(root, "p.pkg"))
        Fx.age(root)
        var now = 1_800_000_000.0
        let lib = Library(root: root, clock: { now })
        lib.scan()
        let first = lib.generatedAt
        now += 100
        lib.scan()
        XCTAssertEqual(lib.generatedAt, first)
        Fx.ps4Pkg(joinPath(root, "q.pkg"), titleId: "CUSA00002")
        Fx.age(root)
        now += 100
        lib.scan()
        XCTAssertNotEqual(lib.generatedAt, first)
    }

    func testSummaryFields() {
        Fx.ps5Folder(root)
        Fx.age(root)
        let lib = Library(root: root)
        lib.scan()
        let s = lib.items()[0].summary()
        XCTAssertEqual(Set(s.keys), ["id", "revision", "platform", "kind", "content", "title", "title_id", "content_id",
                                    "version", "required_firmware", "size", "file_count", "parent_title_id",
                                    "warnings", "added_at", "icon"])
        XCTAssertTrue(s["parent_title_id"] is NSNull)
        XCTAssertEqual(s["icon"] as? String, "/api/v1/items/\(s["id"]!)/icon")
        let json = String(decoding: JSON.data(s), as: UTF8.self)
        XCTAssertTrue(json.contains(#""icon":"/api/v1/items/"#), json)
    }

    // MARK: perfis e estado

    func testBuiltinProfilesMatchRepository() throws {
        for (name, text) in Profiles.builtin {
            let repo = try String(contentsOfFile: joinPath(joinPath(Fx.repoRoot, "profiles"), name), encoding: .utf8)
            XCTAssertEqual(text + "\n", repo, "atualize Profiles.builtin com profiles/\(name)")
        }
        let dir = Fx.tempDir("perfis")
        Profiles.seed(directory: dir)
        XCTAssertEqual(Profiles.load(directory: dir), Profiles.load(directory: joinPath(Fx.repoRoot, "profiles")))
    }

    func testProfileSelection() {
        let ps = profiles
        var r = Profiles.select(ps, platform: "ps5", firmware: "13.60")
        XCTAssertEqual(r.profile?.firmware, "13.60")
        XCTAssertFalse(r.guessed)
        r = Profiles.select(ps, platform: "ps5", firmware: "14.00")
        XCTAssertEqual(r.profile?.firmware, "13.60")
        XCTAssertFalse(r.guessed)
        r = Profiles.select(ps, platform: "ps5", firmware: "9.60")
        XCTAssertEqual(r.profile?.firmware, "13.60")
        XCTAssertTrue(r.guessed)
        XCTAssertNil(Profiles.select(ps, platform: "ps5", firmware: nil).profile)
        XCTAssertNil(Profiles.select(ps, platform: "switch", firmware: "19.0").profile)
    }

    func testStates() {
        Fx.ps4Pkg(joinPath(root, "base.pkg"), appVer: "01.00")
        Fx.ps4Pkg(joinPath(root, "upd.pkg"), category: "gp", version: "01.07")
        Fx.ps4Pkg(joinPath(root, "novo.pkg"), titleId: "CUSA00077")
        Fx.ps4Pkg(joinPath(root, "exige.pkg"), titleId: "CUSA00088", systemVer: 0x14000000)
        Fx.write(joinPath(root, "ps5.pkg"), PKG.build(contentId: "UP0000-PPSA00001_00-ABCDEFGHIJKLMNOP"))
        Fx.write(joinPath(root, "PPSA00002.ffpfs"), [0])
        Fx.age(root)
        let lib = Library(root: root)
        lib.scan()
        let installed: [[String: Any]] = [
            ["title_id": "CUSA00001", "version": "01.00", "content": "base"],
            ["title_id": "cusa00001", "version": "01.05", "content": "update"],
        ]
        let console: [String: Any] = ["platform": "ps5", "firmware": "13.60", "storages": [], "installed": installed]
        var out: [String: [String: Any]] = [:]
        for d in ConsoleState.annotate(lib.items(platform: "ps5"), console: console, profiles: profiles) {
            out["\(d["title_id"]!):\(d["content"]!)"] = d
        }
        XCTAssertEqual(out["CUSA00001:base"]?["state"] as? String, "installed")
        XCTAssertEqual(out["CUSA00001:update"]?["state"] as? String, "update_available")
        XCTAssertEqual(out["CUSA00077:base"]?["state"] as? String, "new")
        XCTAssertEqual(out["CUSA00088:base"]?["state"] as? String, "incompatible")
        XCTAssertTrue((out["CUSA00088:base"]?["state_reason"] as? String ?? "").contains("14.00"))
        XCTAssertEqual(out["PPSA00001:unknown"]?["state"] as? String, "incompatible")
        XCTAssertTrue((out["PPSA00001:unknown"]?["state_reason"] as? String ?? "").contains(".pkg de PS5"))
        XCTAssertEqual(out["PPSA00002:base"]?["state"] as? String, "new")
        XCTAssertTrue((out["PPSA00002:base"]?["warnings"] as? [String] ?? []).contains("experimental_format"))
        XCTAssertTrue(out["CUSA00077:base"]?["state_reason"] is NSNull)
    }

    func testStateWithoutProfileOrFirmware() {
        Fx.ps4Pkg(joinPath(root, "base.pkg"))
        Fx.age(root)
        let lib = Library(root: root)
        lib.scan()
        let d = ConsoleState.annotate(lib.items(), console: ["platform": "ps5", "installed": [] as [Any]], profiles: [])[0]
        XCTAssertEqual(d["state"] as? String, "new")
        XCTAssertTrue(d["state_reason"] is NSNull)
    }
}
