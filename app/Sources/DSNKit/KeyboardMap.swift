/// The keyboard layout, by key position (labels of a US layout):
///
/// | Key            | DS     |
/// |----------------|--------|
/// | arrows         | D-pad  |
/// | X              | A      |
/// | Z              | B      |
/// | S              | X      |
/// | A              | Y      |
/// | Q              | L      |
/// | W              | R      |
/// | Return, Enter  | Start  |
/// | right Shift    | Select |
public enum KeyboardMap {
    /// macOS virtual key codes (`kVK_*`, `NSEvent.keyCode`).
    public static let macKeyCodes: [UInt16: DSButtons] = [
        0x7E: .up, 0x7D: .down, 0x7B: .left, 0x7C: .right,
        0x07: .a,           // X
        0x06: .b,           // Z
        0x01: .x,           // S
        0x00: .y,           // A
        0x0C: .l,           // Q
        0x0D: .r,           // W
        0x24: .start,       // Return
        0x4C: .start,       // keypad Enter
        0x3C: .select,      // right Shift (a modifier: arrives as flagsChanged)
    ]

    /// USB HID keyboard usages (`UIKey.keyCode`, `GCKeyCode`), for hardware keyboards on
    /// iPhone, iPad and Apple TV.
    public static let hidUsages: [Int: DSButtons] = [
        0x52: .up, 0x51: .down, 0x50: .left, 0x4F: .right,
        0x1B: .a,           // X
        0x1D: .b,           // Z
        0x16: .x,           // S
        0x04: .y,           // A
        0x14: .l,           // Q
        0x1A: .r,           // W
        0x28: .start,       // Return
        0x58: .start,       // keypad Enter
        0xE5: .select,      // right Shift
    ]

    /// For a help screen: each key and what it does.
    public static let summary: [(key: String, button: String)] = [
        ("Arrow keys", "D-pad"), ("X", "A"), ("Z", "B"), ("S", "X"), ("A", "Y"),
        ("Q", "L"), ("W", "R"), ("Return", "Start"), ("Right Shift", "Select"),
        ("Mouse / trackpad", "Touch screen (bottom screen)"),
    ]
}
