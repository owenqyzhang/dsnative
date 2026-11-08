import Foundation
import Testing
@testable import DSNKit

/// The paced emulation thread on a real recompiled game: the one linked into this test
/// binary, whose ROM and arm9.bin come from the environment (tests/test_app.py sets them for
/// the homebrew test ROM). One test only: a process can hold one core.
struct EmulatorTests {
    static let game: (rom: URL, arm9: URL)? = {
        let env = ProcessInfo.processInfo.environment
        guard let rom = env["DSN_TEST_ROM"], let arm9 = env["DSN_TEST_ARM9"] else { return nil }
        return (URL(fileURLWithPath: rom), URL(fileURLWithPath: arm9))
    }()

    @Test(.enabled(if: EmulatorTests.game != nil, "set DSN_TEST_ROM and DSN_TEST_ARM9 (tests/test_app.py does)"))
    func pacesPausesAndSteps() throws {
        let game = try #require(Self.game)
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("dsn-emulator-test-\(getpid())")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let saveURL = dir.appendingPathComponent("test.sav")
        // the test ROM (tests/hw/save_9.c) writes a 512 KB FLASH chip, as expect_save.txt says
        let session = try GameSession.start(rom: game.rom, arm9: game.arm9, save: saveURL,
                                            options: [("--save-type", "flash512k"), ("--rtc", "2026-11-07T12:34:56")])
        #expect(session.saveURL == saveURL && session.unknownOptions.isEmpty)
        let e = session.emulator
        e.start()
        let t0 = HostClock.now()
        Thread.sleep(forTimeInterval: 1)
        let ran = e.currentStatus.frame, elapsed = HostClock.now() - t0
        // never faster than the DS; and moving (loosely: the machine may be busy)
        #expect(Double(ran) <= elapsed * FramePacer.dsFrameRate + 2)
        #expect(ran >= 10)

        // the game has written its save by now; flushSave puts it on disk without stopping
        let saveDeadline = HostClock.now() + 5
        while !FileManager.default.fileExists(atPath: saveURL.path) && HostClock.now() < saveDeadline {
            e.flushSave()
            Thread.sleep(forTimeInterval: 0.1)
        }
        let saved = try Data(contentsOf: saveURL)
        #expect(saved.count == 0x80000)
        #expect(saved.contains { $0 != 0xFF })

        // the audio ring, as far as this paced run goes: whatever the mixer has produced, then
        // silence, and never a sample out of range. Order, the channel split, the scale and the
        // wraps are tests/unit/test_audio_ring.c's; that the right amount of real sound arrives
        // (about 548 frames per video frame, the test ROM's tones) is checked by
        // tests/test_app.py's smoke run against dsn_headless's WAV.
        var left = [Float](repeating: 9, count: 512), right = left
        let real = e.core.audioTap.read(left: &left, right: &right, frames: 512)
        #expect((0...512).contains(real))
        #expect(left[real...].allSatisfy { $0 == 0 } && right[real...].allSatisfy { $0 == 0 })
        #expect(left[..<real].allSatisfy { abs($0) <= 1 } && right[..<real].allSatisfy { abs($0) <= 1 })

        e.isPaused = true
        Thread.sleep(forTimeInterval: 0.1)      // a frame under way finishes
        let paused = e.currentStatus.frame
        Thread.sleep(forTimeInterval: 0.2)
        #expect(e.currentStatus.frame == paused)
        e.stepFrame()
        let deadline = HostClock.now() + 2
        while e.currentStatus.frame == paused && HostClock.now() < deadline { Thread.sleep(forTimeInterval: 0.01) }
        Thread.sleep(forTimeInterval: 0.1)
        #expect(e.currentStatus.frame == paused + 1)

        e.stop()
        // the test ROM's top screen (a Mandelbrot) has more than one colour by now
        let pixels = e.latestFrame()
        let top = Set(UnsafeBufferPointer(start: pixels, count: Core.screenWidth * Core.screenHeight))
        #expect(top.count > 4)
        #expect(e.currentStatus.stoppedARM9 == false && e.currentStatus.stoppedARM7 == false)
        e.core.shutdown()
    }
}
