import AVFoundation

/// The game's sound through AVAudioEngine. A source node pulls 32768 Hz stereo from the
/// core's ring (filled once per frame from nds_audio_take); the engine resamples to the
/// device. Silence until the runtime's sound mixer produces samples.
///
/// It keeps playing across what the system does to an engine: a change of output device
/// (macOS, or headphones plugged in) stops the engine and is answered with a restart, and on
/// iOS and tvOS an interruption (a call, Siri, another app taking the session) ends the same
/// way once the system says playback may resume.
@MainActor
public final class AudioOutput {
    private let engine = AVAudioEngine()
    nonisolated(unsafe) private var observers: [NSObjectProtocol] = []
    /// False while the app has paused the sound (the game is paused or in the background).
    private var wantsSound = true

    public init(tap: AudioTap) throws {
        guard let format = AVAudioFormat(standardFormatWithSampleRate: Core.audioRate, channels: 2) else {
            throw Core.LoadError(description: "no 32768 Hz stereo audio format")
        }
        let source = Self.makeSource(tap: tap, format: format)
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        observe()
        try start()
    }

    /// The source node pulling from the core's ring. Built outside the main actor on purpose: a
    /// closure made in a main-actor method is itself main-actor isolated, and the runtime traps
    /// when the audio thread calls it.
    private nonisolated static func makeSource(tap: AudioTap, format: AVAudioFormat) -> AVAudioSourceNode {
        // runs on the real-time audio thread: no locks, no allocation, just the ring
        AVAudioSourceNode(format: format) { isSilence, _, frameCount, bufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard buffers.count >= 2,
                  let left = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let right = buffers[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            if tap.read(left: left, right: right, frames: Int(frameCount)) == 0 { isSilence.pointee = true }
            return noErr
        }
    }

    deinit {
        for o in observers { NotificationCenter.default.removeObserver(o) }
    }

    /// Plays (or resumes) the sound.
    public func start() throws {
        wantsSound = true
        try activateSession()
        if !engine.isRunning { try engine.start() }
    }

    /// Stops the engine; `start()` plays again.
    public func stop() {
        wantsSound = false
        engine.stop()
    }

    // MARK: system events

    private func observe() {
        let center = NotificationCenter.default
        func restart(_ name: Notification.Name, object: AnyObject?, when condition: @escaping @Sendable (Notification) -> Bool = { _ in true }) {
            observers.append(center.addObserver(forName: name, object: object, queue: .main) { [weak self] note in
                guard condition(note) else { return }
                MainActor.assumeIsolated { self?.restartIfWanted() }
            })
        }
        // the output device changed (or its format): the engine has stopped itself
        restart(.AVAudioEngineConfigurationChange, object: engine)
        #if os(iOS) || os(tvOS)
        let session = AVAudioSession.sharedInstance()
        restart(AVAudioSession.interruptionNotification, object: session) { note in
            guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: raw) == .ended else { return false }
            let options = (note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt).map(AVAudioSession.InterruptionOptions.init)
            return options?.contains(.shouldResume) ?? false
        }
        // the audio daemon restarted: the engine's nodes are gone with it
        restart(AVAudioSession.mediaServicesWereResetNotification, object: session)
        #endif
    }

    private func restartIfWanted() {
        guard wantsSound else { return }
        do {
            try activateSession()
            try engine.start()
        } catch {
            // still interrupted (or no output at all): the next event tries again
        }
    }

    private func activateSession() throws {
        #if os(iOS)
        // ambient: follows the silent switch and mixes with the user's music
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.ambient)
        try session.setActive(true)
        #elseif os(tvOS)
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback)
        try session.setActive(true)
        #endif
    }
}
