/// Scripted input in dsn_headless's syntax, so smoke runs can replay what tests/test_hw.py does.
///
/// `keys` is `F:KEYS,...`: from frame F on, KEYS are held (`a b x y l r`, `s` Start, `e`
/// Select, `U D W E` up, down, left, right; an empty set releases everything). `touch` is
/// `F:X:Y,...`: from frame F the stylus is at X, Y on the bottom screen (X < 0 lifts it).
public struct InputScript: Sendable, Equatable {
    public struct Error: Swift.Error, CustomStringConvertible {
        public let description: String
    }

    public private(set) var keys: [Int: DSButtons] = [:]
    public private(set) var touches: [Int: TouchState] = [:]

    public init(keys: String = "", touch: String = "") throws {
        for entry in keys.split(separator: ",") {
            let parts = entry.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2, let frame = Int(parts[0]), frame >= 0 else {
                throw Error(description: "bad key script entry \"\(entry)\" (want FRAME:KEYS)")
            }
            var held: DSButtons = []
            for ch in parts[1] {
                guard let b = Self.letters[ch] else { throw Error(description: "unknown key \"\(ch)\" in \"\(entry)\"") }
                held.insert(b)
            }
            self.keys[frame] = held
        }
        for entry in touch.split(separator: ",") {
            let parts = entry.split(separator: ":").map { Int($0) }
            guard parts.count == 3, let frame = parts[0], let x = parts[1], let y = parts[2], frame >= 0 else {
                throw Error(description: "bad touch script entry \"\(entry)\" (want FRAME:X:Y)")
            }
            touches[frame] = x < 0 ? .up : .at(x, y)
        }
    }

    public var isEmpty: Bool { keys.isEmpty && touches.isEmpty }

    /// What changes at the start of `frame`: the buttons held from then on, and the stylus.
    public func changes(at frame: Int) -> (keys: DSButtons?, touch: TouchState?) {
        (keys[frame], touches[frame])
    }

    static let letters: [Character: DSButtons] = [
        "a": .a, "b": .b, "x": .x, "y": .y, "l": .l, "r": .r, "s": .start, "e": .select,
        "U": .up, "D": .down, "W": .left, "E": .right,
    ]
}
