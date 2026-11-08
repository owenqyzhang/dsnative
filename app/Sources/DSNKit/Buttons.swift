import DSNCore

/// DS buttons, as the runtime's `KEY_*` bits (dsn_core.c checks the two agree).
public struct DSButtons: OptionSet, Sendable, Hashable {
    public let rawValue: UInt16
    public init(rawValue: UInt16) { self.rawValue = rawValue }

    public static let a = DSButtons(rawValue: UInt16(DSN_KEY_A))
    public static let b = DSButtons(rawValue: UInt16(DSN_KEY_B))
    public static let select = DSButtons(rawValue: UInt16(DSN_KEY_SELECT))
    public static let start = DSButtons(rawValue: UInt16(DSN_KEY_START))
    public static let right = DSButtons(rawValue: UInt16(DSN_KEY_RIGHT))
    public static let left = DSButtons(rawValue: UInt16(DSN_KEY_LEFT))
    public static let up = DSButtons(rawValue: UInt16(DSN_KEY_UP))
    public static let down = DSButtons(rawValue: UInt16(DSN_KEY_DOWN))
    public static let r = DSButtons(rawValue: UInt16(DSN_KEY_R))
    public static let l = DSButtons(rawValue: UInt16(DSN_KEY_L))
    public static let x = DSButtons(rawValue: UInt16(DSN_KEY_X))
    public static let y = DSButtons(rawValue: UInt16(DSN_KEY_Y))
}

/// The stylus on the bottom screen, in screen pixels (0-255, 0-191).
public struct TouchState: Sendable, Equatable {
    public var down: Bool
    public var x: Int
    public var y: Int

    public init(down: Bool, x: Int, y: Int) {
        self.down = down
        self.x = x
        self.y = y
    }

    public static let up = TouchState(down: false, x: 0, y: 0)

    public static func at(_ x: Int, _ y: Int) -> TouchState { TouchState(down: true, x: x, y: y) }
}
