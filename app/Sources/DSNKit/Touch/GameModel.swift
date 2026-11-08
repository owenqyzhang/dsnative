#if os(iOS) || os(tvOS)
import Foundation
import Observation

/// The game of an iPhone, iPad or Apple TV app: loaded from the app bundle, with its audio,
/// controllers and view settings, and the rules for when it runs.
///
/// It runs while the app is in front and the player hasn't paused it. Leaving the foreground
/// pauses it, silences it and writes the save: iOS may end a background app without a word.
@MainActor
@Observable
public final class GameModel {
    public enum Phase {
        case loading
        case playing
        case failed(String)
    }

    public private(set) var phase = Phase.loading
    public private(set) var session: GameSession?
    public private(set) var gameName = "DS game"
    /// Paused by the player (the menu), as opposed to by leaving the foreground.
    public private(set) var userPaused = false
    /// A game controller (not the Siri Remote) is connected: the on-screen pad steps aside.
    public private(set) var hasGamepad = false
    public private(set) var stopped: String?

    public var layoutOptions: PlayfieldLayout.Options { didSet { saveSettings() } }
    public var smoothing: Bool { didSet { saveSettings() } }

    @ObservationIgnored private var audio: AudioOutput?
    @ObservationIgnored private let controllers = GameControllers()
    @ObservationIgnored private var inForeground = true
    @ObservationIgnored private var statusTimer: Timer?
    @ObservationIgnored private let defaults = UserDefaults.standard

    public init() {
        let d = UserDefaults.standard
        var o = PlayfieldLayout.Options()
        o.landscape = d.string(forKey: "landscape").flatMap(ScreenLayout.Arrangement.init(rawValue:))
        o.showPad = d.object(forKey: "showPad") as? Bool ?? true
        o.integerScaling = d.bool(forKey: "integerScaling")
        layoutOptions = o
        smoothing = d.bool(forKey: "smoothing")
    }

    private func saveSettings() {
        defaults.set(layoutOptions.landscape?.rawValue, forKey: "landscape")
        defaults.set(layoutOptions.showPad, forKey: "showPad")
        defaults.set(layoutOptions.integerScaling, forKey: "integerScaling")
        defaults.set(smoothing, forKey: "smoothing")
    }

    /// Whether the pad is on screen: wanted, and no controller to make it unnecessary.
    public var showsPad: Bool { layoutOptions.showPad && !hasGamepad }

    // MARK: lifetime

    /// Loads and starts the game from the app bundle. Safe to call again (does nothing).
    public func start() {
        guard session == nil else { return }
        do {
            guard let files = GameSession.bundledGame() else {
                throw Core.LoadError(description: "this app has no game: build it with tools/build_app.py")
            }
            let info = try GameInfo(rom: files.rom)
            let save = try GameSession.dataDirectory().appendingPathComponent(info.saveFileName)
            let session = try GameSession.start(rom: files.rom, arm9: files.arm9, save: save, options: GameSession.bundleOptions())
            self.session = session
            gameName = info.displayName
            do {
                audio = try AudioOutput(tap: session.emulator.core.audioTap)
            } catch {
                // no sound is better than no game
            }
            hasGamepad = controllers.hasGamepad
            session.emulator.start()
            applyRunState()
            phase = .playing
            statusTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.tick() }
            }
        } catch {
            phase = .failed("\(error)")
        }
    }

    /// The app came to the front or left it.
    public func setForeground(_ foreground: Bool) {
        inForeground = foreground
        applyRunState()
        if !foreground {
            session?.emulator.setButtons([], from: .onScreen)
            session?.emulator.setTouch(.up)
            session?.emulator.flushSave()
        }
    }

    public func togglePause() {
        userPaused.toggle()
        applyRunState()
        if userPaused { session?.emulator.flushSave() }
    }

    private func applyRunState() {
        guard let session else { return }
        let run = inForeground && !userPaused
        session.emulator.isPaused = !run
        if run { try? audio?.start() } else { audio?.stop() }
    }

    private func tick() {
        let st = session?.emulator.currentStatus
        stopped = (st?.stoppedARM9 ?? false) || (st?.stoppedARM7 ?? false)
            ? "\([st?.stoppedARM9 == true ? "ARM9" : nil, st?.stoppedARM7 == true ? "ARM7" : nil].compactMap { $0 }.joined(separator: " and ")) stopped"
            : nil
        let pad = controllers.hasGamepad
        if pad != hasGamepad { hasGamepad = pad }
    }

    // MARK: input

    /// At each display refresh: controllers and keyboards are polled, not waited for.
    public func displayTick() {
        session?.emulator.setButtons(controllers.poll(), from: .controller)
    }

    public func padChanged(_ held: DSButtons) {
        session?.emulator.setButtons(held, from: .onScreen)
    }

    public func touchChanged(_ touch: TouchState) {
        session?.emulator.setTouch(touch)
    }

    /// Writes the save and ends the emulation thread (before the app goes away).
    public func shutDown() {
        statusTimer?.invalidate()
        audio?.stop()
        guard let emulator = session?.emulator else { return }
        emulator.stop()
        emulator.core.shutdown()
    }
}
#endif
