import CoreGraphics

/// The on-screen controls of an iPhone or iPad: where they are and which buttons a set of
/// fingers holds. Pure geometry, so it is tested on the Mac; `PadView` draws it and feeds it
/// the touches.
///
/// Coordinates are points in the pad view, which covers the whole window: a finger that lands
/// outside every control isn't the pad's (`contains`), so the bottom screen below can have it.
public struct PadGeometry: Sendable, Equatable {
    public enum Shape: Sendable, Equatable {
        case circle(center: CGPoint, radius: CGFloat)
        case pill(CGRect)
    }

    public enum Kind: Sendable, Equatable {
        case dpad
        case button(DSButtons, label: String)
    }

    public struct Control: Sendable, Equatable {
        public var kind: Kind
        public var shape: Shape
    }

    public private(set) var controls: [Control] = []

    /// Where the controls go: the window and the part of it the pad may use.
    public struct Space: Sendable, Equatable {
        public var size: CGSize
        public var safe: Insets
        /// Whether the screens sit above the pad (portrait, with room below them) or fill the
        /// middle of the window and the pad takes its sides.
        public var zone: CGRect
        public var portrait: Bool

        public init(size: CGSize, safe: Insets, zone: CGRect, portrait: Bool) {
            self.size = size
            self.safe = safe
            self.zone = zone
            self.portrait = portrait
        }
    }

    public init(_ space: Space) {
        let size = space.size, safe = space.safe, zone = space.zone
        let d = min(size.width, size.height)
        // face buttons and the d-pad are as big as a thumb wants, within what the window allows
        let r = min(max(d * 0.075, 24), 38)
        let offset = r * 1.75
        let cluster = offset + r
        let margin = max(16, d * 0.04)
        let pillH = max(30, r * 0.95), shoulderW = r * 3.2, smallW = r * 2.6

        let left = safe.left + margin + cluster, right = size.width - safe.right - margin - cluster
        var cy: CGFloat
        let shoulderY: CGFloat, startY: CGFloat
        if space.portrait {
            shoulderY = zone.minY + 8
            startY = min(zone.maxY, size.height - safe.bottom) - pillH - 10
            // the clusters sit between the shoulders and the start and select row
            let top = shoulderY + pillH + 8, bottom = startY - 8
            cy = (top + bottom) / 2
            cy = min(max(cy, top + cluster), max(top + cluster, bottom - cluster))
        } else {
            shoulderY = safe.top + 8
            cy = size.height - safe.bottom - margin - cluster
            // select and start above the clusters
            startY = cy - cluster - pillH - 10
        }

        func circle(_ c: CGPoint, _ rad: CGFloat) -> Shape { .circle(center: c, radius: rad) }
        controls.append(Control(kind: .dpad, shape: circle(CGPoint(x: left, y: cy), cluster)))
        let fc = CGPoint(x: right, y: cy)
        let face: [(DSButtons, String, CGFloat, CGFloat)] = [
            (.a, "A", offset, 0), (.b, "B", 0, offset), (.x, "X", 0, -offset), (.y, "Y", -offset, 0),
        ]
        for (button, label, dx, dy) in face {
            controls.append(Control(kind: .button(button, label: label), shape: circle(CGPoint(x: fc.x + dx, y: fc.y + dy), r)))
        }
        func pill(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ button: DSButtons, _ label: String) {
            controls.append(Control(kind: .button(button, label: label), shape: .pill(CGRect(x: x, y: y, width: w, height: pillH))))
        }
        pill(safe.left + margin, shoulderY, shoulderW, .l, "L")
        pill(size.width - safe.right - margin - shoulderW, shoulderY, shoulderW, .r, "R")
        if space.portrait {
            let mid = size.width / 2, gap = 12.0
            pill(mid - gap / 2 - smallW, startY, smallW, .select, "SELECT")
            pill(mid + gap / 2, startY, smallW, .start, "START")
        } else {
            // over the d-pad and the face buttons, as far in as the clusters' centres
            pill(left - smallW / 2, startY, smallW, .select, "SELECT")
            pill(right - smallW / 2, startY, smallW, .start, "START")
        }
    }

    /// How far outside a control a finger still counts: thumbs are not precise.
    static let slop: CGFloat = 10

    /// Whether a finger landing at `point` belongs to the pad.
    public func contains(_ point: CGPoint) -> Bool {
        controls.contains { Self.hit($0.shape, point, slop: Self.slop) }
    }

    /// The buttons held by fingers at `points`. Each finger counts for what it is on right now,
    /// so sliding from one direction to another, or from A to B, changes the button.
    public func buttons(at points: [CGPoint]) -> DSButtons {
        var held: DSButtons = []
        for p in points {
            for control in controls {
                switch (control.kind, control.shape) {
                case let (.dpad, .circle(center, radius)):
                    held.formUnion(Self.direction(of: p, from: center, radius: radius))
                case let (.button(button, _), shape):
                    if Self.hit(shape, p, slop: Self.slop) { held.insert(button) }
                default:
                    break
                }
            }
        }
        return held
    }

    /// The d-pad's eight directions by the finger's angle from the centre, within the circle
    /// (a little beyond it) and outside a dead zone in the middle.
    static func direction(of p: CGPoint, from center: CGPoint, radius: CGFloat) -> DSButtons {
        let dx = p.x - center.x, dy = p.y - center.y
        let distance = (dx * dx + dy * dy).squareRoot()
        guard distance <= radius + slop, distance >= radius * 0.2 else { return [] }
        let angle = atan2(-dy, dx) * 180 / .pi          // 0 right, 90 up
        var held: DSButtons = []
        if abs(angle) < 67.5 { held.insert(.right) }
        if abs(angle) > 112.5 { held.insert(.left) }
        if angle > 22.5 && angle < 157.5 { held.insert(.up) }
        if angle < -22.5 && angle > -157.5 { held.insert(.down) }
        return held
    }

    static func hit(_ shape: Shape, _ p: CGPoint, slop: CGFloat) -> Bool {
        switch shape {
        case let .circle(c, r):
            return hypot(p.x - c.x, p.y - c.y) <= r + slop
        case let .pill(rect):
            return rect.insetBy(dx: -slop, dy: -slop).contains(p)
        }
    }
}

/// Distances from the edges of a window to what the system keeps free (notch, home indicator).
public struct Insets: Sendable, Equatable {
    public var top, left, bottom, right: CGFloat

    public init(top: CGFloat = 0, left: CGFloat = 0, bottom: CGFloat = 0, right: CGFloat = 0) {
        self.top = top
        self.left = left
        self.bottom = bottom
        self.right = right
    }

    public static let zero = Insets()
}
