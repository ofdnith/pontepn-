import XCTest
@testable import PonteCore

final class FormatsTests: XCTestCase {
    // MARK: versões

    func testFirmwareBCD() {
        XCTAssertEqual(Versions.firmwareFromBCD(0x05050000, widthBytes: 4), "5.05")
        XCTAssertEqual(Versions.firmwareFromBCD(0x10010000, widthBytes: 4), "10.01")
        XCTAssertEqual(Versions.firmwareFromHexText("0x0403000000000000"), "4.03")
        XCTAssertEqual(Versions.firmwareFromHexText("0x1360000000000000"), "13.60")
        XCTAssertNil(Versions.firmwareFromHexText("lixo"))
        XCTAssertNil(Versions.firmwareFromBCD(0, widthBytes: 4))
    }

    func testCompareVersions() {
        XCTAssertEqual(Versions.compareVersions("01.000.000", "1.0.0", platform: "ps5"), 0)
        XCTAssertEqual(Versions.compareVersions("01.05", "01.4", platform: "ps5"), 1)
        XCTAssertEqual(Versions.compareVersions("01.02", "01.10", platform: "ps5"), -1)
        XCTAssertEqual(Versions.compareVersions("65536", "131072", platform: "switch"), -1)
        XCTAssertEqual(Versions.compareVersions(nil, "1.0", platform: "ps5"), 0)
        XCTAssertEqual(Versions.compareVersions("1.x", "1.0", platform: "ps5"), 0)
        XCTAssertEqual(Versions.compareFirmware("13.60", "9.60"), 1)
        XCTAssertEqual(Versions.compareFirmware("4.03", "4.03"), 0)
        XCTAssertNil(Versions.compareFirmware("13", "4.03"))
    }

    // MARK: SHA-256 e utilidades

    func testSHA256Vectors() {
        XCTAssertEqual(Hasher256.hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        XCTAssertEqual(Hasher256.hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        XCTAssertEqual(Hasher256.hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
        // em pedaços de tamanhos variados, atravessando os blocos de 64 bytes
        var h = Hasher256()
        let million = [UInt8](repeating: 0x61, count: 1_000_000)
        var i = 0
        var step = 1
        while i < million.count {
            let end = min(million.count, i + step)
            h.update(Array(million[i..<end]))
            i = end
            step = step % 97 + 13
        }
        XCTAssertEqual(h.hex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")
    }

    func testItemIdMatchesReference() {
        // mesmo valor que hashlib.sha256(b"ps5:PPSA01234").hexdigest()[:16]
        XCTAssertEqual(Library.itemId(platform: "ps5", rel: "PPSA01234"), String(Hasher256.hex("ps5:PPSA01234").prefix(16)))
        XCTAssertEqual(Library.itemId(platform: "ps5", rel: "PPSA01234").count, 16)
    }

    func testPathSuffix() {
        XCTAssertEqual(pathSuffix("Jogo.nsp"), ".nsp")
        XCTAssertEqual(pathSuffix("a.tar.gz"), ".gz")
        XCTAssertEqual(pathSuffix(".nsp"), "")
        XCTAssertEqual(pathSuffix("semponto"), "")
        XCTAssertEqual(pathSuffix("fim."), "")
    }

    func testISOTime() {
        XCTAssertEqual(ISOTime.format(1_800_000_000.7), "2027-01-15T08:00:00Z")
        XCTAssertEqual(ISOTime.parse("2027-01-15T08:00:00Z"), 1_800_000_000)
        XCTAssertNil(ISOTime.parse("ontem"))
        XCTAssertNil(ISOTime.parse("2027-01-15T08:00:00+00:00"))
    }

    // MARK: param.sfo

    func testSFORoundtrip() throws {
        let data = SFO.build(["TITLE": .string("Ação"), "CATEGORY": .string("gp"), "SYSTEM_VER": .int(0x09000000)])
        XCTAssertEqual(try SFO.parse(data), ["TITLE": .string("Ação"), "CATEGORY": .string("gp"), "SYSTEM_VER": .int(0x09000000)])
    }

    func testSFORejectsGarbage() {
        XCTAssertThrowsError(try SFO.parse(Array("not an sfo at all....".utf8)))
    }

    // MARK: .pkg

    func testPkgPS4() throws {
        let dir = Fx.tempDir()
        let p = Fx.ps4Pkg(joinPath(dir, "x.pkg"), category: "gp", version: "01.05")
        let info = try PKG.read(path: p)
        XCTAssertEqual(info.titleId, "CUSA00001")
        XCTAssertEqual(info.kind, "pkg_ps4")
        XCTAssertEqual(info.sfo?["CATEGORY"], .string("gp"))
        XCTAssertEqual(info.icon, Data(Fx.png))
    }

    func testPkgPS5ByPrefix() throws {
        let p = Fx.write(joinPath(Fx.tempDir(), "y.pkg"), PKG.build(contentId: "UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP"))
        let info = try PKG.read(path: p)
        XCTAssertEqual(info.kind, "pkg_ps5")
        XCTAssertNil(info.sfo)
    }

    func testPkgBadMagic() {
        let p = Fx.write(joinPath(Fx.tempDir(), "z.pkg"), [UInt8](repeating: 0, count: 0x200))
        XCTAssertThrowsError(try PKG.read(path: p))
    }

    func testPkgEntryOutOfBoundsIsIgnored() throws {
        var data = PKG.build(contentId: "UP0000-CUSA00001_00-ABCDEFGHIJKLMNOP",
                             sfo: SFO.build(["CATEGORY": .string("gd")]))
        data.putBE(10_000_000, at: 0x1000 + 20)
        let p = Fx.write(joinPath(Fx.tempDir(), "o.pkg"), data)
        XCTAssertNil(try PKG.read(path: p).sfo)
    }

    // MARK: param.json

    func testParamJsonFallbackLanguage() throws {
        let raw = #"{"titleId":"PPSA1","localizedParameters":{"defaultLanguage":"xx","en-US":{"titleName":"Game"}}}"#
        let meta = try PS5Param.parse(Data(raw.utf8))
        XCTAssertEqual(meta.title, "Game")
        XCTAssertNil(meta.version)
        XCTAssertNil(meta.requiredFirmware)
    }

    func testParamJsonBOM() throws {
        let raw = Data([0xEF, 0xBB, 0xBF] + Array(#"{"titleId":"PPSA00001"}"#.utf8))
        XCTAssertEqual(try PS5Param.parse(raw).titleId, "PPSA00001")
        XCTAssertThrowsError(try PS5Param.parse(Data("[1]".utf8)))
    }

    // MARK: Switch

    func testSwitchClassify() {
        let cases: [(String, String, String?)] = [
            ("0100ABCD12340000", "base", nil),
            ("0100ABCD12340800", "update", "0100ABCD12340000"),
            ("0100ABCD12341001", "dlc", "0100ABCD12340000"),
            ("0100ABCD12341FFF", "dlc", "0100ABCD12340000"),
            ("0100ABCD12341000", "unknown", nil),
            ("0100ABCD12340123", "unknown", nil),
        ]
        for (tid, content, parent) in cases {
            let r = SwitchTID.classify(tid)
            XCTAssertEqual(r.content, content, tid)
            XCTAssertEqual(r.parent, parent, tid)
        }
    }

    func testSwitchNames() {
        let name = "Jogo Legal [0100ABCD12340800][v131072] (BR).nsp"
        XCTAssertEqual(SwitchTID.tidFromName(name), "0100ABCD12340800")
        XCTAssertEqual(SwitchTID.versionFromName(name), "131072")
        XCTAssertEqual(SwitchTID.versionFromName("x [v065536]"), "65536")
        XCTAssertEqual(SwitchTID.cleanTitle("Jogo Legal [0100ABCD12340800][v131072] (BR)"), "Jogo Legal (BR)")
        XCTAssertEqual(SwitchTID.cleanTitle("[0100ABCD12340800]"), "[0100ABCD12340800]")
    }

    func testPFS0Ticket() throws {
        let p = Fx.nsp(joinPath(Fx.tempDir(), "a.nsp"))
        let names = try PFS0.list(path: p).map(\.name)
        XCTAssertEqual(SwitchTID.tidsFromTickets(names), ["0100ABCD12340000"])
    }

    func testPFS0Bad() {
        let p = Fx.write(joinPath(Fx.tempDir(), "b.nsp"), Array("PFS0".utf8) + [UInt8](repeating: 0xFF, count: 12))
        XCTAssertThrowsError(try PFS0.list(path: p))
    }

    // MARK: compactados

    private func names(_ ns: String...) -> [String] { ns.map { "/lib/" + $0 } }

    func testArchiveRarPartsComplete() {
        let sets = Archives.group(names("Jogo.part1.rar", "Jogo.part2.rar", "Jogo.part3.rar"))
        XCTAssertEqual(sets.count, 1)
        XCTAssertEqual(sets[0].key, "Jogo.rar")
        XCTAssertTrue(sets[0].complete)
        XCTAssertEqual(sets[0].firstPart, "/lib/Jogo.part1.rar")
    }

    func testArchiveGapDetected() {
        let sets = Archives.group(names("Jogo.7z.001", "Jogo.7z.003"))
        XCTAssertEqual(sets[0].missing, [2])
        XCTAssertEqual(sets[0].firstPart, "/lib/Jogo.7z.001")
    }

    func testArchiveOldRar() {
        var sets = Archives.group(names("Jogo.rar", "Jogo.r00", "Jogo.r01"))
        XCTAssertEqual(sets.count, 1)
        XCTAssertTrue(sets[0].complete)
        XCTAssertEqual(sets[0].firstPart, "/lib/Jogo.rar")
        sets = Archives.group(names("Jogo.rar", "Jogo.r01"))
        XCTAssertEqual(sets[0].missing, [1])
    }

    func testArchiveSplitZipAndSingles() {
        let sets = Archives.group(names("A.zip", "A.z01", "A.z02", "B.7z", "C.tar.gz", "leia.txt"))
        XCTAssertEqual(sets.map(\.key), ["A.zip", "B.7z", "C.tar.gz"])
        XCTAssertTrue(sets.allSatisfy(\.complete))
        XCTAssertEqual(sets[0].firstPart, "/lib/A.zip")
    }

    // MARK: plano do .exfat

    func testExfatClusterChoice() {
        let mib = ExfatPlan.mib
        XCTAssertEqual(ExfatPlan.make(fileSizes: [2 * mib, 2 * mib], dirCount: 1).cluster, ExfatPlan.clusterLarge)
        XCTAssertEqual(ExfatPlan.make(fileSizes: [1000, 2000], dirCount: 1).cluster, ExfatPlan.clusterSmall)
        XCTAssertEqual(ExfatPlan.make(fileSizes: [1000], dirCount: 1, force64K: true).cluster, ExfatPlan.clusterLarge)
        XCTAssertEqual(ExfatPlan.make(fileSizes: [], dirCount: 0).cluster, ExfatPlan.clusterSmall)
    }

    func testExfatSizeSmallGameUsesMinimumSpare() {
        let mib = ExfatPlan.mib
        let p = ExfatPlan.make(fileSizes: [Int64](repeating: 1024, count: 10), dirCount: 2)
        let allocated: Int64 = 10 * 32 * 1024
        let base: Int64 = allocated + 40 + 2 + 12 * 256 + 32 * mib
        XCTAssertEqual(p.sizeMiB, (base + 64 * mib + mib - 1) / mib)
    }

    func testExfatSizeLargeGameSpareCapped() {
        let sizes = [Int64](repeating: 1 << 30, count: 200)
        let p = ExfatPlan.make(fileSizes: sizes, dirCount: 10)
        let raw = sizes.reduce(0, +)
        XCTAssertEqual(p.cluster, ExfatPlan.clusterLarge)
        XCTAssertGreaterThanOrEqual(p.sizeBytes, raw + 64 * ExfatPlan.mib)
        XCTAssertLessThanOrEqual(Double(p.sizeBytes), Double(raw) * 1.001 + Double(600 * ExfatPlan.mib))
    }

    func testExfatPlanForFolder() throws {
        let root = Fx.ps5Folder(Fx.tempDir(), extra: ["data/sub/x.bin": [1, 2, 3]])
        let p = try ExfatPlan.make(folder: root)
        XCTAssertEqual(p.fileCount, 5)
        XCTAssertEqual(p.dirCount, 3)  // sce_sys, data, data/sub
        XCTAssertEqual(p.rawBytes, 5000 + 1234 + 3 + Int64(Fx.png.count) + Int64(try Data(contentsOf: URL(fileURLWithPath: joinPath(root, "sce_sys/param.json"))).count))
    }
}
