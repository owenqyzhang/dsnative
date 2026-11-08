import Foundation

/// What the app needs from a ROM's header.
public struct GameInfo: Sendable, Equatable {
    public let title: String
    public let gameCode: String

    /// Reads the header's title (0x00, 12 bytes) and game code (0x0C, 4 bytes).
    public init(header: Data) throws {
        guard header.count >= 16 else { throw Core.LoadError(description: "not a DS ROM: the header is too short") }
        let bytes = [UInt8](header.prefix(16))
        func ascii(_ s: ArraySlice<UInt8>) -> String {
            String(decoding: s.prefix { $0 != 0 }.map { (0x20..<0x7F).contains($0) ? $0 : UInt8(ascii: "?") }, as: UTF8.self)
        }
        title = ascii(bytes[0..<12]).trimmingCharacters(in: .whitespaces)
        gameCode = ascii(bytes[12..<16])
    }

    public init(rom: URL) throws {
        let h = try FileHandle(forReadingFrom: rom)
        defer { try? h.close() }
        try self.init(header: h.readData(ofLength: 16))
    }

    public var displayName: String { title.isEmpty ? (gameCode.isEmpty ? "DS game" : gameCode) : title }

    /// A file name for this game's save: the game code, or the title without odd characters.
    public var saveFileName: String {
        let base = gameCode.allSatisfy({ $0.isLetter || $0.isNumber }) && gameCode.count == 4 ? gameCode : displayName
        let safe = String(base.map { $0.isLetter || $0.isNumber || $0 == "-" ? $0 : "_" })
        return safe + ".sav"
    }
}

/// A game ready to run: the core booted, its save file attached when the runtime has save
/// support, and the emulator around it (not started).
public struct GameSession: Sendable {
    public let info: GameInfo
    public let emulator: Emulator
    /// The save file in use, or nil when none was asked for or the runtime refused `--save`.
    public let saveURL: URL?
    /// Runtime options no subsystem knew (reported, not fatal, in the app).
    public let unknownOptions: [String]

    /// The ROM and arm9.bin that tools/build_app.py puts in the bundle's resources.
    public static func bundledGame(in bundle: Bundle = .main) -> (rom: URL, arm9: URL)? {
        guard let rom = bundle.url(forResource: "game", withExtension: "nds"),
              let arm9 = bundle.url(forResource: "arm9", withExtension: "bin") else { return nil }
        return (rom, arm9)
    }

    /// The runtime options the bundle asks for: tools/build_app.py's --save-type, kept in the
    /// Info.plist as DSNSaveType. `--save-type` given at launch wins over it.
    public static func bundleOptions(in bundle: Bundle = .main, given: [(String, String)] = []) -> [(String, String)] {
        guard let type = bundle.object(forInfoDictionaryKey: "DSNSaveType") as? String, !type.isEmpty,
              !given.contains(where: { $0.0 == "--save-type" }) else { return given }
        return given + [("--save-type", type)]
    }

    /// Where saves live. macOS: Application Support/<bundle id>/. iOS and iPadOS: the app's
    /// Documents, which the Files app can show (Info.plist) so a save can be taken out and put
    /// back. tvOS has no Documents: Application Support, which the system may clear when the
    /// Apple TV runs short of space.
    public static func dataDirectory(bundle: Bundle = .main) throws -> URL {
        let fm = FileManager.default
        #if os(iOS)
        let dir = try fm.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        #else
        let base = try fm.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        let dir = base.appendingPathComponent(bundle.bundleIdentifier ?? "dsnative", isDirectory: true)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        #endif
        return dir
    }

    /// Loads and boots a game. `save` is the save file to use (nil for none); `options` are
    /// runtime options as name/value pairs, applied before boot.
    public static func start(rom: URL, arm9: URL, save: URL?, options: [(String, String)] = []) throws -> GameSession {
        let info = try GameInfo(rom: rom)
        let core = try Core(rom: rom, arm9: arm9)
        var unknown: [String] = []
        for (name, value) in options where !core.option(name, value) { unknown.append(name) }
        // a runtime without save support (no --save) just runs without one
        let saveURL = save.flatMap { core.option("--save", $0.path) ? $0 : nil }
        core.boot()
        return GameSession(info: info, emulator: Emulator(core: core), saveURL: saveURL, unknownOptions: unknown)
    }
}
