import Foundation
@testable import PonteCore
import XCTest

final class ProgressTests: XCTestCase {
    func testRateMeterMeasuresRecentSpeed() {
        var m = RateMeter()
        let t0 = Date(timeIntervalSince1970: 1_000)
        for i in 0...4 { m.add(total: Int64(i) * 10_000_000, at: t0.addingTimeInterval(Double(i))) }
        // 40 MB em 4 s
        XCTAssertEqual(m.bytesPerSecond(at: t0.addingTimeInterval(4)), 10_000_000, accuracy: 1)
        // parado: zero, não a média antiga
        XCTAssertEqual(m.bytesPerSecond(at: t0.addingTimeInterval(8)), 0)
    }

    func testRateMeterEmptyIsZero() {
        XCTAssertEqual(RateMeter().bytesPerSecond(), 0)
    }

    func testHashProgressFractionAndTimeLeft() {
        let h = Library.HashProgress(title: "Jogo", filesLeft: 2, doneBytes: 50, totalBytes: 200, bytesPerSecond: 50)
        XCTAssertEqual(h.fraction, 0.25)
        XCTAssertEqual(h.secondsLeft, 3)
    }

    func testHashFileReportsProgress() throws {
        let dir = NSTemporaryDirectory() + "ponte-hash-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let path = dir + "/a.bin"
        try Data(repeating: 7, count: 9 << 20).write(to: URL(fileURLWithPath: path))
        var seen: [Int64] = []
        XCTAssertNotNil(Library.hashFile(path) { seen.append($0) })
        XCTAssertEqual(seen.last, 9 << 20)
        XCTAssertGreaterThan(seen.count, 1)
    }

    func testLastMatch() {
        XCTAssertEqual(RX(#"(\d{1,3})%"#).lastMatch("  3% 1\u{8}\u{8}  17% 2 - a\u{8}\u{8} 42% 3 - b"), "42")
        XCTAssertNil(RX(#"(\d+)%"#).lastMatch("nada"))
    }
}

final class HashToggleTests: XCTestCase {
    /// Conferência desligada: nenhum hash calculado nem informado aos consoles.
    func testHashesDisabled() throws {
        let dir = NSTemporaryDirectory() + "ponte-nohash-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        Fx.nsp(joinPath(dir, "Jogo [0100ABCD12340000][v0].nsp"))
        let lib = Library(root: dir, stableSeconds: 0)
        lib.hashesEnabled = false
        lib.scan()
        XCTAssertEqual(lib.hashPending(), 0)
        let f = try XCTUnwrap(lib.items().first?.files.first)
        XCTAssertNil(lib.sha256(f))
        // ligada de novo: calcula
        lib.hashesEnabled = true
        _ = lib.hashPending()
        XCTAssertNotNil(lib.sha256(f))
    }
}
