import Foundation

/// Runs a `Core` on a thread of its own at the DS frame rate and hands finished frames to the
/// display, so the UI thread never waits for the game.
///
/// `step()` is the frame: input in, one frame run, both screens published. The paced thread
/// and smoke mode (`run(frames:script:)`) both go through it.
public final class Emulator: @unchecked Sendable {
    /// Where buttons come from; the game sees all of them combined.
    public enum Source: Int, CaseIterable, Sendable {
        case keyboard, controller, onScreen, script
    }

    public struct Status: Sendable, Equatable {
        public var frame = 0
        public var paused = false
        public var stoppedARM9 = false
        public var stoppedARM7 = false
        /// Frames run per second over the last second or so (0 until measured).
        public var framesPerSecond = 0.0
    }

    public let core: Core

    // Everything below is guarded by `lock`, except `buffers[writeIndex]`, which only the
    // thread running `step()` touches.
    private let lock = NSCondition()
    private var buttons = [DSButtons](repeating: [], count: Source.allCases.count)
    private var pointerTouch = TouchState.up    // mouse or finger
    private var scriptTouch = TouchState.up
    private var status = Status()
    private var stepsPending = 0
    private var running = false
    private var flushPending = false
    private let finished = DispatchSemaphore(value: 0)
    private let flushed = DispatchSemaphore(value: 0)
    // Triple buffering: the producer fills `write` then swaps it with `ready`; the display
    // swaps `ready` with `read` when a newer frame is there.
    private let buffers: [UnsafeMutablePointer<UInt32>]
    private var writeIndex = 0, readyIndex = 1, readIndex = 2
    private var readySerial = 0, readSerial = 0
    private var rateWindowStart = 0.0, rateWindowFrames = 0

    public init(core: Core) {
        self.core = core
        buffers = (0..<3).map { _ in
            let p = UnsafeMutablePointer<UInt32>.allocate(capacity: Core.pixelCount)
            p.initialize(repeating: 0xFF00_0000, count: Core.pixelCount)
            return p
        }
    }

    deinit {
        for b in buffers { b.deallocate() }
    }

    // MARK: input

    public func setButtons(_ held: DSButtons, from source: Source) {
        lock.withLock { buttons[source.rawValue] = held }
    }

    /// The stylus from a pointer (mouse, trackpad or finger); `.up` lifts it.
    public func setTouch(_ touch: TouchState) {
        lock.withLock { pointerTouch = touch }
    }

    // MARK: the frame

    /// Runs one frame with the current input and publishes both screens.
    public func step() {
        let (held, touch) = lock.withLock { () -> (DSButtons, TouchState) in
            (buttons.reduce([]) { $0.union($1) }, pointerTouch.down ? pointerTouch : scriptTouch)
        }
        core.runFrame(buttons: held, touch: touch)
        core.copyScreens(into: buffers[writeIndex])
        let stopped = core.stopped, frame = core.frameCount, now = HostClock.now()
        lock.withLock {
            swap(&writeIndex, &readyIndex)
            readySerial += 1
            status.frame = frame
            status.stoppedARM9 = stopped.arm9
            status.stoppedARM7 = stopped.arm7
            rateWindowFrames += 1
            if rateWindowStart == 0 { rateWindowStart = now }
            if now - rateWindowStart >= 1 {
                status.framesPerSecond = Double(rateWindowFrames) / (now - rateWindowStart)
                rateWindowStart = now
                rateWindowFrames = 0
            }
        }
    }

    /// The newest finished frame: both screens, top first, `Core.pixelCount` words. It stays
    /// valid and unchanged until the next call; one consumer (the display) calls this.
    public func latestFrame() -> UnsafePointer<UInt32> {
        lock.withLock {
            if readySerial > readSerial {
                swap(&readyIndex, &readIndex)
                readSerial = readySerial
            }
            return UnsafePointer(buffers[readIndex])
        }
    }

    public var currentStatus: Status { lock.withLock { status } }

    /// Runs `frames` frames as fast as possible, with scripted input, on the calling thread
    /// (smoke tests); `afterFrame` runs on that thread after each frame, before the next one
    /// (the smoke run drains the audio ring there). Stops early when both CPUs have stopped,
    /// as dsn_headless does.
    public func run(frames: Int, script: InputScript, afterFrame: (() -> Void)? = nil) {
        var held: DSButtons = []
        for f in 0..<frames {
            let change = script.changes(at: f)
            if let k = change.keys { held = k }
            let touch = change.touch
            lock.withLock {
                buttons[Source.script.rawValue] = held
                if let touch { scriptTouch = touch }
            }
            step()
            afterFrame?()
            let st = core.stopped
            if st.arm9 && st.arm7 { break }
        }
    }

    // MARK: the paced thread

    public var isPaused: Bool {
        get { lock.withLock { status.paused } }
        set {
            lock.withLock {
                status.paused = newValue
                stepsPending = 0
                lock.broadcast()
            }
        }
    }

    /// While paused, runs one more frame.
    public func stepFrame() {
        lock.withLock {
            guard status.paused else { return }
            stepsPending += 1
            lock.broadcast()
        }
    }

    /// Writes the save file between two frames, and returns once it is on disk: for the moment
    /// the app goes to the background, where iOS may end it without a word. The paced thread
    /// also does it every few seconds, so a crash costs little.
    public func flushSave() {
        let direct = lock.withLock { () -> Bool in
            guard running else { return true }
            flushPending = true
            lock.broadcast()
            return false
        }
        if direct {
            core.flushSave()
        } else {
            _ = flushed.wait(timeout: .now() + 5)
        }
    }

    public func start() {
        lock.withLock {
            precondition(!running, "the emulator is already running")
            running = true
        }
        let thread = Thread { [self] in loop() }
        thread.name = "dsnative.emulation"
        thread.qualityOfService = .userInteractive
        thread.start()
    }

    /// Ends the thread and waits for it, so the core can be shut down safely.
    public func stop() {
        let wasRunning = lock.withLock { () -> Bool in
            let r = running
            running = false
            lock.broadcast()
            return r
        }
        if wasRunning {
            finished.wait()
            // a flush asked for as the thread ended
            if lock.withLock({ flushPending }) { core.flushSave() }
        }
    }

    /// How often (in frames, about 10 s) the paced thread saves a changed save file.
    static let autosaveFrames = 600

    private func loop() {
        var pacer = FramePacer()
        var sinceSave = 0
        while true {
            lock.lock()
            while running && status.paused && stepsPending == 0 && !flushPending { lock.wait() }
            if flushPending {
                flushPending = false
                lock.unlock()
                core.flushSave()
                sinceSave = 0
                flushed.signal()
                continue
            }
            guard running else { lock.unlock(); break }
            let single = status.paused
            if single { stepsPending -= 1 }
            lock.unlock()

            step()
            sinceSave += 1
            if sinceSave >= Self.autosaveFrames {
                sinceSave = 0
                core.flushSave()
            }
            if single {
                pacer.reset()
                continue
            }
            HostClock.sleep(until: pacer.nextDeadline(now: HostClock.now()))
        }
        finished.signal()
    }
}
