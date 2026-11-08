import CoreGraphics
import Metal
import Testing
@testable import DSNKit

/// The Metal path without a window: a known frame drawn offscreen and read back.
@MainActor
@Suite(.enabled(if: MTLCreateSystemDefaultDevice() != nil, "needs a Metal device"))
struct RendererTests {
    static let w = Core.screenWidth, h = Core.screenHeight

    /// Both screens with every pixel different: gradients, opaque, top screen first.
    let frame: [UInt32] = (0..<Core.pixelCount).map { i in
        let screen = UInt32(i / (w * h)), x = UInt32(i % w), y = UInt32(i / w % h)
        return screen == 0 ? 0xFF00_0000 | x << 16 | y << 8 | 0x40 : 0xFF00_0000 | y << 16 | x << 8 | 0x80
    }

    func render(_ layout: ScreenLayout, _ width: Int, _ height: Int) throws -> [UInt32] {
        let r = try ScreenRenderer(device: MTLCreateSystemDefaultDevice()!)
        r.layout = layout
        return try frame.withUnsafeBufferPointer { try r.render($0.baseAddress!, width: width, height: height) }
    }

    @Test func stackedAtOneXIsTheFramebuffer() throws {
        #expect(try render(ScreenLayout(), Self.w, 2 * Self.h) == frame)
    }

    @Test func sideBySideAtTwoX() throws {
        let out = try render(ScreenLayout(arrangement: .sideBySide), 4 * Self.w, 2 * Self.h)
        var bad = 0
        for y in 0..<2 * Self.h {
            for x in 0..<4 * Self.w {
                let screen = x / (2 * Self.w), sx = x % (2 * Self.w) / 2, sy = y / 2
                if out[y * 4 * Self.w + x] != frame[screen * Self.w * Self.h + sy * Self.w + sx] { bad += 1 }
            }
        }
        #expect(bad == 0)
    }

    @Test func marginsAreBlack() throws {
        // 1x in a wider view: the screens are centred, 172 px of black either side
        let width = Self.w + 344
        let out = try render(ScreenLayout(), width, 2 * Self.h)
        #expect(out[0] == 0xFF00_0000 && out[width - 1] == 0xFF00_0000 && out[171] == 0xFF00_0000)
        #expect(out[172] == frame[0])
        #expect(out[(2 * Self.h - 1) * width + 172 + Self.w - 1] == frame[Core.pixelCount - 1])
    }
}
