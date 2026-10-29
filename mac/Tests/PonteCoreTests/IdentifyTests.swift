import XCTest
@testable import PonteCore

final class IdentifyTests: XCTestCase {
    var dir = ""

    override func setUp() { dir = Fx.tempDir("id") }
    override func tearDown() { try? FileManager.default.removeItem(atPath: dir) }

    func identify(_ path: String) -> Identification? { Identifier.identify(path) }

    func testPS5Folder() {
        let r = identify(Fx.ps5Folder(dir))!.item
        XCTAssertEqual([r.platform, r.kind, r.content], ["ps5", "folder", "base"])
        XCTAssertEqual(r.title, "Jogo Cinco")
        XCTAssertEqual(r.titleId, "PPSA01234")
        XCTAssertEqual(r.version, "01.000.000")
        XCTAssertEqual(r.requiredFirmware, "4.03")
        XCTAssertEqual(r.iconPath.map(lastComponent), "icon0.png")
        XCTAssertEqual(r.warnings, [])
    }

    func testPS5FolderWithoutEboot() {
        guard case .attention(let a) = identify(Fx.ps5Folder(dir, eboot: false)) else { return XCTFail() }
        XCTAssertTrue(a.reason.contains("eboot.bin"))
    }

    func testPS5FolderBrokenJson() {
        let root = Fx.ps5Folder(dir)
        Fx.write(joinPath(root, "sce_sys/param.json"), "{quebrado")
        XCTAssertTrue(identify(root)!.isAttention)
    }

    func testPS4Folder() {
        let root = joinPath(dir, "CUSA00002")
        Fx.write(joinPath(root, "sce_sys/param.sfo"), SFO.build([
            "CATEGORY": .string("gd"), "TITLE": .string("Velho"), "TITLE_ID": .string("CUSA00002"),
            "APP_VER": .string("01.02"), "SYSTEM_VER": .int(0x04550000)]))
        let r = identify(root)!.item
        XCTAssertEqual(r.kind, "folder_ps4")
        XCTAssertEqual(r.content, "base")
        XCTAssertEqual(r.title, "Velho")
        XCTAssertEqual(r.version, "01.02")
        XCTAssertEqual(r.requiredFirmware, "4.55")
    }

    func testPlainFolderAndRandomFileAreNotItems() {
        try! FileManager.default.createDirectory(atPath: joinPath(dir, "Downloads"), withIntermediateDirectories: true)
        XCTAssertNil(identify(joinPath(dir, "Downloads")))
        XCTAssertNil(identify(Fx.write(joinPath(dir, "leia.txt"), "oi")))
        XCTAssertNil(identify(joinPath(dir, "nao-existe.nsp")))
    }

    func testPS5Images() {
        for (ext, kind) in [(".exfat", "exfat"), (".ffpkg", "ffpkg"), (".ffpfs", "ffpfs"), (".FFPFSC", "ffpfsc")] {
            let p = Fx.write(joinPath(dir, "Jogo Bom PPSA05678\(ext)"), [UInt8](repeating: 0, count: 100))
            let r = identify(p)!.item
            XCTAssertEqual([r.platform, r.kind, r.titleId], ["ps5", kind, "PPSA05678"])
            XCTAssertEqual(r.title, "Jogo Bom PPSA05678")
            XCTAssertTrue(r.warnings.contains("metadata_from_filename"))
        }
    }

    func testImageWithoutTitleId() {
        let r = identify(Fx.write(joinPath(dir, "qualquer.exfat"), [0]))!.item
        XCTAssertNil(r.titleId)
        XCTAssertEqual(r.title, "qualquer")
        // Title ID colado em outro código não conta
        XCTAssertNil(identify(Fx.write(joinPath(dir, "XPPSA056789.exfat"), [0]))!.item.titleId)
    }

    func testPkgPS4BaseUpdateDlc() {
        let base = identify(Fx.ps4Pkg(joinPath(dir, "b.pkg"), category: "gd", appVer: "01.00"))!.item
        let upd = identify(Fx.ps4Pkg(joinPath(dir, "u.pkg"), category: "gp", appVer: "01.00", version: "01.07"))!.item
        let dlc = identify(Fx.ps4Pkg(joinPath(dir, "d.pkg"), category: "ac"))!.item
        XCTAssertEqual(base.kind, "pkg_ps4")
        XCTAssertEqual(base.content, "base")
        XCTAssertEqual(base.version, "01.00")
        XCTAssertNil(base.parentTitleId)
        XCTAssertEqual(upd.content, "update")
        XCTAssertEqual(upd.version, "01.07")
        XCTAssertEqual(upd.parentTitleId, "CUSA00001")
        XCTAssertEqual(dlc.content, "dlc")
        XCTAssertEqual(base.requiredFirmware, "5.05")
        XCTAssertEqual(base.iconData, Data(Fx.png))
        XCTAssertEqual(base.title, "Jogo Quatro")
    }

    func testPkgPS5() {
        let p = Fx.write(joinPath(dir, "jogo.pkg"), PKG.build(contentId: "UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP"))
        let r = identify(p)!.item
        XCTAssertEqual([r.kind, r.titleId, r.title], ["pkg_ps5", "PPSA01234", "jogo"])
        XCTAssertTrue(r.warnings.contains("metadata_from_filename"))
    }

    func testPkgGarbageNeedsAttention() {
        let p = Fx.write(joinPath(dir, "x.pkg"), Array(String(repeating: "lixo", count: 100).utf8))
        XCTAssertTrue(identify(p)!.isAttention)
    }

    func testNspTicketWinsOverName() {
        let p = Fx.nsp(joinPath(dir, "Nome Errado [0100FFFF00000000][v0].nsp"), tid: "0100ABCD12340800")
        let r = identify(p)!.item
        XCTAssertEqual([r.platform, r.kind, r.titleId, r.content], ["switch", "nsp", "0100ABCD12340800", "update"])
        XCTAssertEqual(r.parentTitleId, "0100ABCD12340000")
        XCTAssertEqual(r.version, "0")
        XCTAssertEqual(r.title, "Nome Errado")
        XCTAssertFalse(r.warnings.contains("metadata_from_filename"))
    }

    func testNspWithoutTicketUsesName() {
        let p = Fx.nsp(joinPath(dir, "Jogo [0100ABCD12341003][v65536].nsz"), ticket: false)
        let r = identify(p)!.item
        XCTAssertEqual([r.kind, r.content, r.titleId, r.version], ["nsz", "dlc", "0100ABCD12341003", "65536"])
        XCTAssertTrue(r.warnings.contains("metadata_from_filename"))
    }

    func testNspTwoTicketsFallsBackToName() {
        let p = Fx.write(joinPath(dir, "Pacote [0100ABCD12340000].nsp"), PFS0.build([
            ("0100abcd123400000000000000000000.tik", [1]), ("0100abcd123408000000000000000000.tik", [1])]))
        XCTAssertEqual(identify(p)!.item.titleId, "0100ABCD12340000")
    }

    func testXciByNameAndWithoutId() {
        let r = identify(Fx.write(joinPath(dir, "Cartucho [0100AAAA00000000][v0].xci"), [UInt8](repeating: 0, count: 512)))!.item
        XCTAssertEqual([r.kind, r.content, r.titleId], ["xci", "base", "0100AAAA00000000"])
        let s = identify(Fx.write(joinPath(dir, "sem id.xcz"), [0]))!.item
        XCTAssertNil(s.titleId)
        XCTAssertEqual(s.content, "unknown")
    }

    func testSidecarOverridesAndRescues() {
        let img = Fx.write(joinPath(dir, "PPSA01234.exfat"), [0])
        Fx.write(Identifier.sidecarPath(img), #"{"title": "Nome Real", "version": "01.000.000", "required_firmware": "4.03", "kind": "não-existe"}"#)
        Fx.write(joinPath(dir, "PPSA01234.exfat.ponte.png"), Fx.png)
        let r = identify(img)!.item
        XCTAssertEqual([r.kind, r.title, r.version, r.requiredFirmware], ["exfat", "Nome Real", "01.000.000", "4.03"])
        XCTAssertEqual(r.titleId, "PPSA01234")
        XCTAssertEqual(r.warnings, [])
        XCTAssertEqual(r.iconPath.map(lastComponent), "PPSA01234.exfat.ponte.png")
    }

    func testSidecarNullClearsField() {
        let img = Fx.write(joinPath(dir, "PPSA01234.exfat"), [0])
        Fx.write(Identifier.sidecarPath(img), #"{"title_id": null, "title": null}"#)
        let r = identify(img)!.item
        XCTAssertNil(r.titleId)
        XCTAssertEqual(r.title, "PPSA01234.exfat")  // sem título: nome do arquivo
    }

    func testSidecarMakesUnknownFileAnItem() {
        let f = Fx.write(joinPath(dir, "misterio.bin"), [0])
        Fx.write(Identifier.sidecarPath(f), #"{"platform": "ps5", "kind": "ffpkg", "title_id": "PPSA00009"}"#)
        let r = identify(f)!.item
        XCTAssertEqual([r.platform, r.kind, r.titleId], ["ps5", "ffpkg", "PPSA00009"])
    }

    func testSidecarInvalidKeepsAttention() {
        let f = Fx.write(joinPath(dir, "misterio.bin"), [0])
        Fx.write(Identifier.sidecarPath(f), #"{"platform": "xbox"}"#)
        XCTAssertTrue(identify(f)!.isAttention)
    }

    func testSidecarFilesAreNotItems() {
        XCTAssertNil(identify(Fx.write(joinPath(dir, "a.exfat.ponte.json"), "{}")))
        XCTAssertNil(identify(Fx.write(joinPath(dir, "a.exfat.ponte-tmp"), "x")))
    }

    func testSidecarRoundtrip() throws {
        let root = Fx.ps5Folder(dir)
        let r = identify(root)!.item
        let side = Identifier.sidecar(r)
        XCTAssertEqual(side["kind"] as? String, "folder")
        XCTAssertEqual(side["title"] as? String, "Jogo Cinco")
        XCTAssertTrue(side["parent_title_id"] is NSNull)
        // grava ao lado de uma imagem e lê de volta
        let img = Fx.write(joinPath(dir, "PPSA01234.exfat"), [0])
        var asImage = r
        asImage.kind = "exfat"
        try Identifier.writeSidecar(asImage, for: img)
        let back = identify(img)!.item
        XCTAssertEqual([back.kind, back.title, back.version, back.requiredFirmware, back.contentId],
                       ["exfat", "Jogo Cinco", "01.000.000", "4.03", "UP0000-PPSA01234_00-ABCDEFGHIJKLMNOP"])
        XCTAssertEqual(back.warnings, [])
    }
}
