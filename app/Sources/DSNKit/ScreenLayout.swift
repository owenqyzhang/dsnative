import CoreGraphics

/// Where the two screens go in a view, and which touch-screen pixel a point lands on.
/// Coordinates are drawable pixels with the origin at the top left.
public struct ScreenLayout: Sendable, Equatable {
    public enum Arrangement: String, Sendable, CaseIterable {
        case stacked        // top screen above the bottom one, as on the DS
        case sideBySide     // top screen on the left
    }

    public static let screenWidth = 256
    public static let screenHeight = 192

    public var arrangement: Arrangement
    /// Whole multiples of the DS resolution (when at least 1x fits), for even pixels.
    public var integerScaling: Bool

    public init(arrangement: Arrangement = .stacked, integerScaling: Bool = false) {
        self.arrangement = arrangement
        self.integerScaling = integerScaling
    }

    /// Both screens as arranged, at 1x.
    public var contentSize: CGSize {
        let w = CGFloat(Self.screenWidth), h = CGFloat(Self.screenHeight)
        return arrangement == .stacked ? CGSize(width: w, height: 2 * h) : CGSize(width: 2 * w, height: h)
    }

    /// The largest scale that fits `size` with the aspect ratio kept.
    public func scale(in size: CGSize) -> CGFloat {
        let c = contentSize
        let fit = max(0, min(size.width / c.width, size.height / c.height))
        return integerScaling && fit >= 1 ? fit.rounded(.down) : fit
    }

    /// The screens' rectangles in a view of `size`, centred and on whole pixels.
    public func frames(in size: CGSize) -> (top: CGRect, bottom: CGRect) {
        let s = scale(in: size)
        let w = (CGFloat(Self.screenWidth) * s).rounded(), h = (CGFloat(Self.screenHeight) * s).rounded()
        let total = arrangement == .stacked ? CGSize(width: w, height: 2 * h) : CGSize(width: 2 * w, height: h)
        let x0 = ((size.width - total.width) / 2).rounded(.down), y0 = ((size.height - total.height) / 2).rounded(.down)
        let top = CGRect(x: x0, y: y0, width: w, height: h)
        let bottom = arrangement == .stacked ? top.offsetBy(dx: 0, dy: h) : top.offsetBy(dx: w, dy: 0)
        return (top, bottom)
    }

    /// The touch-screen pixel under `point`, or nil off the bottom screen. With `clamp`, the
    /// nearest edge pixel instead: a drag that leaves the screen keeps touching its edge.
    public func touchPoint(at point: CGPoint, in size: CGSize, clamp: Bool = false) -> (x: Int, y: Int)? {
        let r = frames(in: size).bottom
        guard r.width > 0, r.height > 0 else { return nil }
        if !clamp && !r.contains(point) { return nil }
        let x = Int(((point.x - r.minX) / r.width * CGFloat(Self.screenWidth)).rounded(.down))
        let y = Int(((point.y - r.minY) / r.height * CGFloat(Self.screenHeight)).rounded(.down))
        return (min(max(x, 0), Self.screenWidth - 1), min(max(y, 0), Self.screenHeight - 1))
    }
}
