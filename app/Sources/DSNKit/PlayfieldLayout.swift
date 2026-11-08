import CoreGraphics

/// Where everything goes in a window on iOS, iPadOS and tvOS: the two screens, the on-screen
/// pad and the menu button. Pure arithmetic, so it is tested on the Mac.
///
/// - Portrait: the screens stacked at the top, the pad in a strip below them.
/// - Landscape on a phone: the screens stacked in the middle and the pad at the sides, which
///   is where the room is. On an iPad, or by choice: side by side, the pad underneath (or over
///   the corners where the screens leave no room).
/// - The TV: the screens side by side, large, inside the margin the system keeps for overscan.
public struct PlayfieldLayout: Sendable, Equatable {
    public struct Options: Sendable, Equatable {
        /// The arrangement in landscape; nil picks stacked on a phone and side by side otherwise.
        public var landscape: ScreenLayout.Arrangement?
        public var showPad = true
        public var integerScaling = false

        public init(landscape: ScreenLayout.Arrangement? = nil, showPad: Bool = true, integerScaling: Bool = false) {
            self.landscape = landscape
            self.showPad = showPad
            self.integerScaling = integerScaling
        }
    }

    public enum Style: Sendable {
        /// iPhone or iPad: fingers, so a pad.
        case touch
        /// Apple TV: a controller or the remote, no pad; the screens as large as the margin allows.
        case tv
    }

    public var arrangement: ScreenLayout.Arrangement
    /// The rectangle the screens are drawn in (the Metal view's frame).
    public var screens: CGRect
    public var pad: PadGeometry?
    /// The menu button's centre (none on the TV).
    public var menu: CGPoint?

    public var screenLayout: ScreenLayout { ScreenLayout(arrangement: arrangement, integerScaling: false) }

    /// Phones are shorter than this in landscape; stacked screens fit there with the pad at the sides.
    static let compactHeight: CGFloat = 600

    /// The largest the pad strip under portrait screens gets.
    static let maxPadStrip: CGFloat = 300
    static let minPadStrip: CGFloat = 215

    public static func make(size: CGSize, safe: Insets, options: Options = Options(), style: Style = .touch) -> PlayfieldLayout {
        let inner = CGRect(x: safe.left, y: safe.top, width: max(0, size.width - safe.left - safe.right),
                           height: max(0, size.height - safe.top - safe.bottom))
        switch style {
        case .tv:
            let arrangement = ScreenLayout.Arrangement.sideBySide
            let r = fit(arrangement, in: inner, options: options, alignTop: false)
            return PlayfieldLayout(arrangement: arrangement, screens: r, pad: nil, menu: nil)
        case .touch:
            let portrait = size.height >= size.width
            let arrangement: ScreenLayout.Arrangement =
                portrait ? .stacked : options.landscape ?? (size.height < compactHeight ? .stacked : .sideBySide)
            var avail = inner
            var zone = CGRect.zero
            if portrait {
                let strip = options.showPad ? min(max(size.height * 0.30, minPadStrip), maxPadStrip) : 0
                avail.size.height = max(0, inner.height - strip)
            }
            // the screens: centred in what is left in portrait, at the top otherwise (the pad is
            // below or over the corners; a stacked pair fills the height anyway)
            let r = fit(arrangement, in: avail, options: options, alignTop: !portrait)
            if portrait {
                // all of what the screens leave below them (at least the strip)
                zone = CGRect(x: 0, y: r.maxY, width: size.width, height: max(0, size.height - r.maxY))
            }
            var pad: PadGeometry?
            if options.showPad {
                pad = PadGeometry(PadGeometry.Space(size: size, safe: safe, zone: zone, portrait: portrait))
            }
            let menu = portrait ? CGPoint(x: size.width / 2, y: (zone.minY + 8 + 15)) : CGPoint(x: size.width / 2, y: safe.top + 8 + 15)
            return PlayfieldLayout(arrangement: arrangement, screens: r, pad: pad, menu: menu)
        }
    }

    /// The screens' rectangle: as large as `avail` allows with the aspect ratio kept, centred
    /// horizontally, and vertically too unless `alignTop`.
    static func fit(_ arrangement: ScreenLayout.Arrangement, in avail: CGRect, options: Options, alignTop: Bool) -> CGRect {
        let layout = ScreenLayout(arrangement: arrangement, integerScaling: options.integerScaling)
        let scale = layout.scale(in: avail.size)
        let c = layout.contentSize
        let w = (c.width * scale).rounded(.down), h = (c.height * scale).rounded(.down)
        return CGRect(x: (avail.minX + (avail.width - w) / 2).rounded(.down),
                      y: (alignTop ? avail.minY : avail.minY + (avail.height - h) / 2).rounded(.down), width: w, height: h)
    }
}
