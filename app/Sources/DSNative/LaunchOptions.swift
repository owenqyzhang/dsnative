import DSNKit
import Foundation

/// The app's command line. A bundle built by tools/build_app.py needs none; the rest is for
/// development and tests.
struct LaunchOptions {
    struct UsageError: Error, CustomStringConvertible {
        let description: String
    }

    static let usage = """
    usage: DSNative [--rom game.nds --arm9 arm9.bin] [--save file.sav] [--<runtime option> value ...]
           DSNative --smoke N [--png out.png] [--keys F:KEYS,...] [--touch F:X:Y,...] [...]

    Without --rom/--arm9 the game comes from the app bundle's resources (game.nds, arm9.bin).
    --smoke N runs N frames without a window through the same frame loop, prints the game's
    debug output and statistics, and writes both screens to --png (256x384, top screen first).
    --keys and --touch take dsn_headless's input script. Other --name value pairs are runtime
    options (nds_option); the app uses --save itself. Smoke runs use a save file only when
    --save is given; windowed runs default to Application Support/<bundle id>/<game code>.sav.
    """

    var help = false
    var smokeFrames: Int?
    var png: String?
    var keys = ""
    var touch = ""
    var rom: String?
    var arm9: String?
    var save: String?
    var runtime: [(String, String)] = []

    static func parse(_ arguments: [String]) throws -> LaunchOptions {
        var o = LaunchOptions()
        var i = 1
        func value(_ name: String) throws -> String {
            i += 1
            guard i < arguments.count else { throw UsageError(description: "\(name) needs a value") }
            return arguments[i]
        }
        while i < arguments.count {
            let a = arguments[i]
            switch a {
            case "--help", "-h": o.help = true
            case "--smoke":
                guard let n = Int(try value(a)), n >= 0 else { throw UsageError(description: "--smoke needs a frame count") }
                o.smokeFrames = n
            case "--png": o.png = try value(a)
            case "--keys": o.keys = try value(a)
            case "--touch": o.touch = try value(a)
            case "--rom": o.rom = try value(a)
            case "--arm9": o.arm9 = try value(a)
            case "--save": o.save = try value(a)
            default:
                if a.hasPrefix("--") {
                    o.runtime.append((a, try value(a)))
                } else if a.hasPrefix("-psn_") {
                    // a process serial number from older launchers
                } else if a.hasPrefix("-") {
                    // an AppKit user-defaults pair (-NSSomething value)
                    if i + 1 < arguments.count && !arguments[i + 1].hasPrefix("-") { i += 1 }
                } else {
                    throw UsageError(description: "unexpected argument \(a)")
                }
            }
            i += 1
        }
        return o
    }

    /// Runtime options: those given, and the bundle's save type unless one was given.
    var runtimeOptions: [(String, String)] { GameSession.bundleOptions(given: runtime) }

    /// The game to run: from --rom and --arm9, or the bundle's resources.
    func gameFiles() throws -> (rom: URL, arm9: URL) {
        switch (rom, arm9) {
        case let (rom?, arm9?):
            return (URL(fileURLWithPath: rom).standardizedFileURL, URL(fileURLWithPath: arm9).standardizedFileURL)
        case (nil, nil):
            if let bundled = GameSession.bundledGame() { return bundled }
            throw UsageError(description: "no game: run the app bundle tools/build_app.py makes, or pass --rom game.nds --arm9 arm9.bin")
        default:
            throw UsageError(description: "--rom and --arm9 go together")
        }
    }
}

/// Standard error, without touching C's `stderr` global (not concurrency-safe in Swift 6).
func printError(_ message: String) {
    FileHandle.standardError.write(Data((message + "\n").utf8))
}
