import DSNKit
import Foundation

/// `--smoke N`: the game for N frames without a window, through the same frame step the
/// windowed app paces, then both screens to a PNG. For tests and quick checks.
enum Smoke {
    static func run(_ options: LaunchOptions, frames: Int) -> Int32 {
        do {
            let files = try options.gameFiles()
            let script = try InputScript(keys: options.keys, touch: options.touch)
            let save = options.save.map { URL(fileURLWithPath: $0).standardizedFileURL }
            let session = try GameSession.start(rom: files.rom, arm9: files.arm9, save: save, options: options.runtimeOptions)
            if let name = session.unknownOptions.first {
                printError("DSNative: unknown option \(name)")
                return 2
            }
            if save != nil && session.saveURL == nil {
                printError("DSNative: the runtime did not take --save; running without a save file")
            }
            let emulator = session.emulator
            emulator.run(frames: frames, script: script)
            if let png = options.png {
                try PNGWriter.write(emulator.latestFrame(), width: Core.screenWidth, height: Core.screenHeight * 2,
                                    to: URL(fileURLWithPath: png))
            }
            printError(emulator.core.statistics)
            emulator.core.shutdown()
            let stopped = emulator.core.stopped
            return stopped.arm9 || stopped.arm7 ? 1 : 0
        } catch {
            printError("DSNative: \(error)")
            return 2
        }
    }
}
