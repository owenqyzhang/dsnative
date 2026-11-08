import DSNCore
import Foundation

/// One recompiled game on the runtime. One per process: the dispatcher's caches are static.
///
/// Not thread-safe: `Emulator` makes sure one thread at a time drives it. The audio tap is
/// the exception; it reads a lock-free ring on the audio thread.
public final class Core: @unchecked Sendable {
    public struct LoadError: Error, CustomStringConvertible {
        public let description: String
    }

    public static let screenWidth = Int(DSN_SCREEN_W)
    public static let screenHeight = Int(DSN_SCREEN_H)
    /// Both screens, top first.
    public static let pixelCount = screenWidth * screenHeight * 2
    public static let audioRate = Double(DSN_AUDIO_RATE)

    let handle: OpaquePointer

    /// Loads the ROM and the decompressed ARM9 image (arm9.bin) the recompiler wrote.
    public init(rom: URL, arm9: URL) throws {
        var err = [CChar](repeating: 0, count: 512)
        guard let h = dsn_core_create(rom.path, arm9.path, &err, Int32(err.count)) else {
            throw LoadError(description: err.withUnsafeBufferPointer { String(cString: $0.baseAddress!) })
        }
        handle = h
    }

    /// A runtime option such as `--save path`, before `boot()`. False when no subsystem has it.
    public func option(_ name: String, _ value: String) -> Bool {
        dsn_core_option(handle, name, value) != 0
    }

    public func boot() { dsn_core_boot(handle) }

    public func runFrame(buttons: DSButtons, touch: TouchState) {
        dsn_core_set_input(handle, buttons.rawValue, touch.down ? 1 : 0, Int32(touch.x), Int32(touch.y))
        dsn_core_run_frame(handle)
    }

    /// Both screens into `pixels` (`pixelCount` words, 0xAARRGGBB, top screen first).
    public func copyScreens(into pixels: UnsafeMutablePointer<UInt32>) {
        dsn_core_copy_screens(handle, pixels)
    }

    public var frameCount: Int { Int(dsn_core_frame_count(handle)) }

    /// The CPUs that stopped: code nothing was compiled for, or something the runtime lacks.
    public var stopped: (arm9: Bool, arm7: Bool) {
        let s = dsn_core_stopped(handle)
        return (s & 1 != 0, s & 2 != 0)
    }

    /// A line like dsn_headless --stats prints.
    public var statistics: String {
        var buf = [CChar](repeating: 0, count: 512)
        dsn_core_describe(handle, &buf, Int32(buf.count))
        return buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    }

    /// Writes the save file if the game changed it. Not while a frame runs: `Emulator.flushSave()`
    /// picks the moment. True when the file is up to date.
    @discardableResult
    public func flushSave() -> Bool { dsn_core_flush_save(handle) == 0 }

    /// Lets the subsystems write what they keep (the save file). The core stops running.
    public func shutdown() { dsn_core_shutdown(handle) }

    public var audioTap: AudioTap { AudioTap(handle: handle) }
}

/// The audio thread's view of a core: just its sound ring.
public struct AudioTap: @unchecked Sendable {
    let handle: OpaquePointer

    /// Fills both buffers (`frames` floats each), zero-padding what the ring lacks; returns
    /// how many frames were real sound.
    @discardableResult
    public func read(left: UnsafeMutablePointer<Float>, right: UnsafeMutablePointer<Float>, frames: Int) -> Int {
        Int(dsn_core_audio_read(handle, left, right, UInt32(frames)))
    }
}
