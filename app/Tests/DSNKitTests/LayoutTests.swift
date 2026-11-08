import CoreGraphics
import Foundation
import Testing
@testable import DSNKit

struct LayoutTests {
    @Test func stackedFillsATwoTimesWindow() {
        let f = ScreenLayout().frames(in: CGSize(width: 512, height: 768))
        #expect(f.top == CGRect(x: 0, y: 0, width: 512, height: 384))
        #expect(f.bottom == CGRect(x: 0, y: 384, width: 512, height: 384))
    }

    @Test func sideBySidePutsTheTopScreenLeft() {
        let f = ScreenLayout(arrangement: .sideBySide).frames(in: CGSize(width: 1024, height: 384))
        #expect(f.top == CGRect(x: 0, y: 0, width: 512, height: 384))
        #expect(f.bottom == CGRect(x: 512, y: 0, width: 512, height: 384))
    }

    @Test func aspectIsKeptAndCentred() {
        // 1000 x 768: the height limits the scale to 2, leaving 244 px on each side
        let f = ScreenLayout().frames(in: CGSize(width: 1000, height: 768))
        #expect(f.top == CGRect(x: 244, y: 0, width: 512, height: 384))
        #expect(f.bottom.minY == 384)
    }

    @Test func integerScalingRoundsDown() {
        let size = CGSize(width: 700, height: 1000)     // fits 2.6x
        #expect(ScreenLayout().scale(in: size) > 2.6)
        #expect(ScreenLayout(integerScaling: true).scale(in: size) == 2)
        // below 1x there is no whole multiple: it shrinks to fit anyway
        #expect(ScreenLayout(integerScaling: true).scale(in: CGSize(width: 128, height: 192)) == 0.5)
    }

    @Test func touchLandsOnBottomScreenPixels() {
        let layout = ScreenLayout(), size = CGSize(width: 512, height: 768)
        #expect(layout.touchPoint(at: CGPoint(x: 0, y: 384), in: size)! == (0, 0))
        #expect(layout.touchPoint(at: CGPoint(x: 257, y: 384 + 193), in: size)! == (128, 96))
        #expect(layout.touchPoint(at: CGPoint(x: 511.9, y: 767.9), in: size)! == (255, 191))
        // the top screen is not a touch screen
        #expect(layout.touchPoint(at: CGPoint(x: 100, y: 100), in: size) == nil)
        // a drag that leaves the screen holds its edge
        #expect(layout.touchPoint(at: CGPoint(x: 600, y: 100), in: size, clamp: true)! == (255, 0))
    }

    @Test func touchFollowsSideBySide() {
        let layout = ScreenLayout(arrangement: .sideBySide), size = CGSize(width: 512, height: 192)
        #expect(layout.touchPoint(at: CGPoint(x: 100, y: 50), in: size) == nil)
        #expect(layout.touchPoint(at: CGPoint(x: 256 + 10, y: 20), in: size)! == (10, 20))
    }
}

struct InputTests {
    @Test func scriptMatchesTheHeadlessSyntax() throws {
        let s = try InputScript(keys: "35:E,45:ED,55:", touch: "10:100:50,20:-1:0")
        #expect(s.changes(at: 35).keys == [.right])
        #expect(s.changes(at: 45).keys == [.right, .down])
        #expect(s.changes(at: 55).keys == [])
        #expect(s.changes(at: 36).keys == nil)
        #expect(s.changes(at: 10).touch == .at(100, 50))
        #expect(s.changes(at: 20).touch == .up)
        let all = try InputScript(keys: "0:abxylrseUDWE")
        #expect(all.changes(at: 0).keys == [.a, .b, .x, .y, .l, .r, .start, .select, .up, .down, .left, .right])
    }

    @Test func badScriptsAreRefused() {
        #expect(throws: InputScript.Error.self) { try InputScript(keys: "35") }
        #expect(throws: InputScript.Error.self) { try InputScript(keys: "x:a") }
        #expect(throws: InputScript.Error.self) { try InputScript(keys: "3:q") }
        #expect(throws: InputScript.Error.self) { try InputScript(touch: "3:1") }
    }

    @Test func everyButtonHasAKey() {
        let all: DSButtons = [.a, .b, .x, .y, .l, .r, .start, .select, .up, .down, .left, .right]
        #expect(KeyboardMap.macKeyCodes.values.reduce(DSButtons()) { $0.union($1) } == all)
        #expect(KeyboardMap.hidUsages.values.reduce(DSButtons()) { $0.union($1) } == all)
        // the same keys on both: one button per key, as many keys per button
        #expect(KeyboardMap.macKeyCodes.count == KeyboardMap.hidUsages.count)
    }

    @Test func buttonsAreKeyinputBits() {
        #expect(DSButtons.a.rawValue == 1 && DSButtons.select.rawValue == 4 && DSButtons.up.rawValue == 0x40)
        #expect(DSButtons.x.rawValue == 0x400 && DSButtons.y.rawValue == 0x800)
    }
}

struct PacingTests {
    func close(_ a: Double, _ b: Double) -> Bool { abs(a - b) < 1e-9 }

    @Test func dsRate() {
        #expect(abs(FramePacer.dsFrameRate - 59.8261) < 0.0001)
    }

    @Test func deadlinesDontDrift() {
        var p = FramePacer(rate: 50)
        #expect(close(p.nextDeadline(now: 1.0), 1.02))
        // a frame that finished late still aims at the schedule, not at now + period
        #expect(close(p.nextDeadline(now: 1.03), 1.04))
        #expect(close(p.nextDeadline(now: 1.041), 1.06))
    }

    @Test func aStallStartsAfresh() {
        var p = FramePacer(rate: 50, maxLagFrames: 3)
        _ = p.nextDeadline(now: 0)
        // more than three frames behind: no burst of catch-up frames
        #expect(p.nextDeadline(now: 1) == 1)
        #expect(close(p.nextDeadline(now: 1), 1.02))
        p.reset()
        #expect(close(p.nextDeadline(now: 5), 5.02))
    }
}

struct GameInfoTests {
    func header(_ title: String, _ code: String) -> Data {
        var d = Data(title.utf8.prefix(12))
        d.append(Data(repeating: 0, count: 12 - d.count))
        d.append(Data(code.utf8))
        return d
    }

    @Test func readsTitleAndCode() throws {
        let info = try GameInfo(header: header("HOMEBREW 2", "HBRE"))
        #expect(info.title == "HOMEBREW 2" && info.gameCode == "HBRE")
        #expect(info.displayName == "HOMEBREW 2")
        #expect(info.saveFileName == "HBRE.sav")
    }

    @Test func oddCodesFallBackToTheTitle() throws {
        let info = try GameInfo(header: header("MY GAME", "\0\0\0\0"))
        #expect(info.saveFileName == "MY_GAME.sav")
        #expect(throws: Core.LoadError.self) { try GameInfo(header: Data(count: 8)) }
    }
}
