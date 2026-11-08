import CoreGraphics
import Testing
@testable import DSNKit

struct PadTests {
    /// An iPhone in portrait: 402 x 874 points, the notch above and the home indicator below.
    let phone = PlayfieldLayout.make(size: CGSize(width: 402, height: 874), safe: Insets(top: 62, bottom: 34))

    func pad(_ l: PlayfieldLayout) -> PadGeometry { l.pad! }

    func centre(_ g: PadGeometry, _ b: DSButtons) -> CGPoint {
        for c in g.controls {
            if case let .button(button, _) = c.kind, button == b {
                switch c.shape {
                case let .circle(p, _): return p
                case let .pill(r): return CGPoint(x: r.midX, y: r.midY)
                }
            }
        }
        fatalError("no control for \(b)")
    }

    func dpad(_ g: PadGeometry) -> (CGPoint, CGFloat) {
        for c in g.controls where c.kind == .dpad {
            if case let .circle(p, r) = c.shape { return (p, r) }
        }
        fatalError("no d-pad")
    }

    @Test func everyButtonIsOnScreen() {
        let all: DSButtons = [.a, .b, .x, .y, .l, .r, .start, .select, .up, .down, .left, .right]
        for size in [CGSize(width: 402, height: 874), CGSize(width: 874, height: 402), CGSize(width: 834, height: 1194),
                     CGSize(width: 1194, height: 834), CGSize(width: 375, height: 667)] {
            let l = PlayfieldLayout.make(size: size, safe: Insets(top: 24, left: size.width > size.height ? 40 : 0, bottom: 20,
                                                                  right: size.width > size.height ? 40 : 0))
            let g = pad(l)
            // pressing the centre of each control (the d-pad: a point on each side) gives every button
            var held: DSButtons = []
            let (c, r) = dpad(g)
            let points = [CGPoint(x: c.x + r * 0.6, y: c.y), CGPoint(x: c.x - r * 0.6, y: c.y),
                          CGPoint(x: c.x, y: c.y - r * 0.6), CGPoint(x: c.x, y: c.y + r * 0.6)]
            held.formUnion(g.buttons(at: points))
            for b in [DSButtons.a, .b, .x, .y, .l, .r, .start, .select] { held.formUnion(g.buttons(at: [centre(g, b)])) }
            #expect(held == all, "size \(size)")
            // and all of it inside the window
            for control in g.controls {
                let rect: CGRect
                switch control.shape {
                case let .circle(p, rad): rect = CGRect(x: p.x - rad, y: p.y - rad, width: 2 * rad, height: 2 * rad)
                case let .pill(r): rect = r
                }
                #expect(CGRect(origin: .zero, size: size).contains(rect), "\(control) outside \(size)")
            }
        }
    }

    @Test func dpadHasEightDirections() {
        let g = pad(phone)
        let (c, r) = dpad(g)
        func at(_ deg: Double) -> DSButtons {
            g.buttons(at: [CGPoint(x: c.x + r * 0.7 * cos(deg * .pi / 180), y: c.y - r * 0.7 * sin(deg * .pi / 180))])
        }
        #expect(at(0) == [.right] && at(90) == [.up] && at(180) == [.left] && at(270) == [.down])
        #expect(at(45) == [.right, .up] && at(135) == [.left, .up] && at(225) == [.left, .down] && at(315) == [.right, .down])
        // the middle is a dead zone
        #expect(g.buttons(at: [c]) == [])
    }

    @Test func fingersHoldSeveralButtonsAtOnce() {
        let g = pad(phone)
        let (c, r) = dpad(g)
        let held = g.buttons(at: [CGPoint(x: c.x + r * 0.7, y: c.y), centre(g, .a), centre(g, .l)])
        #expect(held == [.right, .a, .l])
        // sliding a finger from A onto B swaps the button, and between them holds both
        #expect(g.buttons(at: [centre(g, .b)]) == [.b])
        let a = centre(g, .a), b = centre(g, .b)
        #expect(g.buttons(at: [CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)]) == [.a, .b])
    }

    @Test func fingersAwayFromTheControlsAreTheScreens() {
        let g = pad(phone)
        let screens = phone.screens
        #expect(!g.contains(CGPoint(x: screens.midX, y: screens.midY)))
        #expect(g.buttons(at: [CGPoint(x: screens.midX, y: screens.midY)]) == [])
        #expect(g.contains(centre(g, .a)) && g.contains(centre(g, .start)))
    }
}

struct PlayfieldTests {
    func make(_ w: CGFloat, _ h: CGFloat, safe: Insets = .zero, _ o: PlayfieldLayout.Options = .init(),
              style: PlayfieldLayout.Style = .touch) -> PlayfieldLayout {
        PlayfieldLayout.make(size: CGSize(width: w, height: h), safe: safe, options: o, style: style)
    }

    @Test func portraitStacksTheScreensAboveThePad() {
        let l = make(402, 874, safe: Insets(top: 62, bottom: 34))
        #expect(l.arrangement == .stacked)
        #expect(l.screens.minY >= 62 && l.screens.width <= 402)
        // the width is the limit: 402 / 256 = 1.57 at a pad strip of 262
        #expect(abs(l.screens.width / l.screens.height - 256.0 / 384.0) < 0.01)
        let g = l.pad!
        for c in g.controls {
            if case let .circle(p, _) = c.shape { #expect(p.y > l.screens.maxY - 1) }
        }
        #expect(l.screens.maxY <= 874 - 34 - PlayfieldLayout.minPadStrip + 1)
    }

    @Test func landscapePhoneStacksWithThePadAtTheSides() {
        let l = make(874, 402, safe: Insets(top: 0, left: 62, bottom: 21, right: 62))
        #expect(l.arrangement == .stacked)
        #expect(l.screens.height <= 402 - 21 && l.screens.width < 874 / 2)
        let g = l.pad!
        for c in g.controls {
            if case let .circle(p, rad) = c.shape {
                #expect(p.x + rad < l.screens.minX || p.x - rad > l.screens.maxX, "clear of the screens")
            }
        }
    }

    @Test func landscapePadsPutScreensSideBySide() {
        let l = make(1194, 834, safe: Insets(top: 24, bottom: 20))
        #expect(l.arrangement == .sideBySide)
        #expect(l.screens.width == 1194 && l.screens.minY == 24)
        // the pad lives under them
        for c in l.pad!.controls {
            if case let .circle(p, _) = c.shape { #expect(p.y > l.screens.maxY) }
        }
    }

    @Test func landscapeChoiceIsHonoured() {
        let l = make(874, 402, safe: Insets(left: 62, bottom: 21, right: 62), .init(landscape: .sideBySide))
        #expect(l.arrangement == .sideBySide)
        #expect(l.screens.width > 700)
        #expect(make(1194, 834, .init(landscape: .stacked)).arrangement == .stacked)
    }

    @Test func noPadMeansBiggerScreens() {
        let with = make(402, 874, safe: Insets(top: 62, bottom: 34))
        let without = make(402, 874, safe: Insets(top: 62, bottom: 34), .init(showPad: false))
        #expect(without.pad == nil && without.screens.width >= with.screens.width)
        #expect(without.screens.height > with.screens.height - 1)
    }

    @Test func tvKeepsTheMarginAndFillsTheWidth() {
        // 1920 x 1080 with the overscan margin tvOS reports: 90 sides, 60 top and bottom
        let l = make(1920, 1080, safe: Insets(top: 60, left: 90, bottom: 60, right: 90), style: .tv)
        #expect(l.arrangement == .sideBySide && l.pad == nil)
        #expect(l.screens.minX >= 90 && l.screens.maxX <= 1830 && l.screens.minY >= 60 && l.screens.maxY <= 1020)
        // the width is the limit: 1740 / 512 = 3.4x
        #expect(l.screens.width >= 1700)
        #expect(abs(l.screens.minX + l.screens.maxX - 1920) <= 1)
    }

    @Test func integerScalingKeepsWholeMultiples() {
        let l = make(402, 874, safe: Insets(top: 62, bottom: 34), .init(integerScaling: true))
        #expect(l.screens.width == 256 || l.screens.width == 512)
    }
}
