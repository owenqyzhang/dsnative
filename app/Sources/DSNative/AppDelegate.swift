import AppKit
import DSNKit
import MetalKit

/// The macOS app: one window with both screens, the emulation and view menus, and the game's
/// lifetime (the save file is written when the app quits).
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, NSMenuItemValidation {
    private let options: LaunchOptions
    private var session: GameSession?
    private var window: NSWindow?
    private var gameView: GameView?
    private var renderer: ScreenRenderer?
    private var audio: AudioOutput?
    private let controllers = GameControllers()
    private var statusTimer: Timer?
    private var settings = ViewSettings.load()

    init(options: LaunchOptions) {
        self.options = options
    }

    // MARK: lifetime

    func applicationDidFinishLaunching(_ notification: Notification) {
        do {
            try startGame()
        } catch {
            let alert = NSAlert()
            alert.messageText = "The game can't start"
            alert.informativeText = "\(error)"
            alert.runModal()
            NSApp.terminate(nil)
        }
    }

    private func startGame() throws {
        let files = try options.gameFiles()
        let info = try GameInfo(rom: files.rom)
        let save = try options.save.map { URL(fileURLWithPath: $0).standardizedFileURL }
            ?? GameSession.dataDirectory().appendingPathComponent(info.saveFileName)
        let session = try GameSession.start(rom: files.rom, arm9: files.arm9, save: save, options: options.runtimeOptions)
        self.session = session
        for name in session.unknownOptions { printError("DSNative: no runtime subsystem knows \(name); ignored") }
        if session.saveURL == nil { printError("DSNative: the runtime did not take --save; progress isn't kept") }

        let view = GameView(frame: NSRect(origin: .zero, size: windowSize(scale: 2)), device: MTLCreateSystemDefaultDevice())
        let renderer = try ScreenRenderer(view: view, emulator: session.emulator)
        renderer.onDisplayFrame = { [controllers] in
            session.emulator.setButtons(controllers.poll(), from: .controller)
        }
        view.emulator = session.emulator
        view.currentLayout = { [unowned renderer] in renderer.layout }
        self.gameView = view
        self.renderer = renderer
        applySettings()

        let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = session.info.displayName
        window.contentView = view
        window.contentMinSize = settings.layout.contentSize
        window.collectionBehavior.insert(.fullScreenPrimary)
        window.delegate = self
        window.isRestorable = false     // no state restoration: a killed run mustn't bring back a minimized window
        window.setFrameAutosaveName("game")
        window.makeFirstResponder(view)
        self.window = window

        buildMenus(title: session.info.displayName)
        do {
            audio = try AudioOutput(tap: session.emulator.core.audioTap)
        } catch {
            printError("DSNative: no audio: \(error)")
        }
        session.emulator.start()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate()
        statusTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.updateStatus() }
        }
        updateStatus()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        statusTimer?.invalidate()
        audio?.stop()
        guard let emulator = session?.emulator else { return }
        // the thread must be gone before the core writes the save and stops for good
        emulator.stop()
        emulator.core.shutdown()
    }

    /// Another app in front: the save goes to disk in case this one is never reopened.
    func applicationDidResignActive(_ notification: Notification) {
        session?.emulator.flushSave()
    }

    func windowDidResignKey(_ notification: Notification) {
        gameView?.releaseAll()
    }

    // MARK: status

    private func updateStatus() {
        guard let session, let window else { return }
        let st = session.emulator.currentStatus
        if st.stoppedARM9 || st.stoppedARM7 {
            let which = [st.stoppedARM9 ? "ARM9" : nil, st.stoppedARM7 ? "ARM7" : nil].compactMap { $0 }.joined(separator: " and ")
            window.subtitle = "\(which) stopped at frame \(st.frame)"
        } else if st.paused {
            window.subtitle = "Paused at frame \(st.frame)"
        } else {
            window.subtitle = st.framesPerSecond > 0 ? String(format: "%.1f fps", st.framesPerSecond) : ""
        }
    }

    // MARK: view settings

    private func applySettings() {
        renderer?.layout = settings.layout
        renderer?.smoothing = settings.smoothing
        window?.contentMinSize = settings.layout.contentSize
        settings.save()
    }

    /// The window's content size at a whole-number scale of the DS screens.
    private func windowSize(scale: CGFloat) -> NSSize {
        let c = settings.layout.contentSize
        return NSSize(width: c.width * scale, height: c.height * scale)
    }

    /// The scale the screens are drawn at now, in points (at least 1).
    private var currentScale: CGFloat {
        guard let view = gameView else { return 2 }
        return max(1, settings.layout.scale(in: view.bounds.size).rounded())
    }

    private func resizeWindow(scale: CGFloat) {
        guard let window, !window.styleMask.contains(.fullScreen) else { return }
        var frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: windowSize(scale: scale)))
        frame.origin = NSPoint(x: window.frame.minX, y: window.frame.maxY - frame.height)    // keep the top left
        window.setFrame(frame, display: true, animate: true)
    }

    @objc private func setStacked(_ sender: Any?) { setArrangement(.stacked) }
    @objc private func setSideBySide(_ sender: Any?) { setArrangement(.sideBySide) }

    private func setArrangement(_ arrangement: ScreenLayout.Arrangement) {
        guard settings.layout.arrangement != arrangement else { return }
        let scale = currentScale
        settings.layout.arrangement = arrangement
        applySettings()
        resizeWindow(scale: scale)
    }

    @objc private func toggleIntegerScaling(_ sender: Any?) {
        settings.layout.integerScaling.toggle()
        applySettings()
    }

    @objc private func toggleSmoothing(_ sender: Any?) {
        settings.smoothing.toggle()
        applySettings()
    }

    @objc private func setWindowScale(_ sender: NSMenuItem) {
        resizeWindow(scale: CGFloat(sender.tag))
    }

    // MARK: emulation

    @objc private func togglePause(_ sender: Any?) {
        guard let emulator = session?.emulator else { return }
        emulator.isPaused.toggle()
        if emulator.isPaused { emulator.flushSave() }
        gameView?.releaseAll()
        updateStatus()
    }

    @objc private func stepFrame(_ sender: Any?) {
        session?.emulator.stepFrame()
        // the status line catches up with the frame on the next tick
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in self?.updateStatus() }
    }

    @objc private func revealSave(_ sender: Any?) {
        guard let url = session?.saveURL else { return }
        if FileManager.default.fileExists(atPath: url.path) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            NSWorkspace.shared.open(url.deletingLastPathComponent())
        }
    }

    @objc private func showControls(_ sender: Any?) {
        let alert = NSAlert()
        alert.messageText = "Controls"
        let keys = KeyboardMap.summary.map { "\($0.key): \($0.button)" }.joined(separator: "\n")
        alert.informativeText = keys + "\n\n" + GameControllers.summary
        alert.runModal()
    }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        let paused = session?.emulator.isPaused ?? false
        switch item.action {
        case #selector(togglePause(_:)):
            item.title = paused ? "Resume" : "Pause"
        case #selector(stepFrame(_:)):
            return paused
        case #selector(setStacked(_:)):
            item.state = settings.layout.arrangement == .stacked ? .on : .off
        case #selector(setSideBySide(_:)):
            item.state = settings.layout.arrangement == .sideBySide ? .on : .off
        case #selector(toggleIntegerScaling(_:)):
            item.state = settings.layout.integerScaling ? .on : .off
        case #selector(toggleSmoothing(_:)):
            item.state = settings.smoothing ? .on : .off
        case #selector(setWindowScale(_:)):
            return !(window?.styleMask.contains(.fullScreen) ?? true)
        case #selector(revealSave(_:)):
            return session?.saveURL != nil
        default:
            break
        }
        return true
    }

    // MARK: menus

    private func buildMenus(title: String) {
        let main = NSMenu()
        func submenu(_ name: String, _ items: [NSMenuItem]) -> NSMenu {
            let menu = NSMenu(title: name)
            for item in items { menu.addItem(item) }
            let holder = NSMenuItem(title: name, action: nil, keyEquivalent: "")
            holder.submenu = menu
            main.addItem(holder)
            return menu
        }
        func item(_ title: String, _ action: Selector, _ key: String = "", _ modifiers: NSEvent.ModifierFlags = .command,
                  target: AnyObject? = nil) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
            i.keyEquivalentModifierMask = modifiers
            i.target = target ?? self
            return i
        }
        let app = NSApplication.shared
        _ = submenu(title, [
            item("About \(title)", #selector(NSApplication.orderFrontStandardAboutPanel(_:)), target: app),
            .separator(),
            item("Hide \(title)", #selector(NSApplication.hide(_:)), "h", target: app),
            item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option], target: app),
            item("Show All", #selector(NSApplication.unhideAllApplications(_:)), target: app),
            .separator(),
            item("Quit \(title)", #selector(NSApplication.terminate(_:)), "q", target: app),
        ])
        _ = submenu("Emulation", [
            item("Pause", #selector(togglePause(_:)), "p"),
            item("Step Frame", #selector(stepFrame(_:)), "]"),
            .separator(),
            item("Show Save File in Finder", #selector(revealSave(_:))),
        ])
        var scales: [NSMenuItem] = []
        for s in 1...4 {
            let i = item("Window Size \(s)x", #selector(setWindowScale(_:)), "\(s)", [.command, .option])
            i.tag = s
            scales.append(i)
        }
        _ = submenu("View", [
            item("Stacked", #selector(setStacked(_:)), "1"),
            item("Side by Side", #selector(setSideBySide(_:)), "2"),
            .separator(),
            item("Integer Scaling", #selector(toggleIntegerScaling(_:)), "i"),
            item("Smooth Pixels", #selector(toggleSmoothing(_:)), "l"),
            .separator(),
        ] + scales + [
            .separator(),
            item("Enter Full Screen", #selector(NSWindow.toggleFullScreen(_:)), "f", [.command, .control], target: nil),
        ])
        let windowMenu = submenu("Window", [
            item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m", target: nil),
            item("Zoom", #selector(NSWindow.performZoom(_:)), target: nil),
            .separator(),
            item("Bring All to Front", #selector(NSApplication.arrangeInFront(_:)), target: app),
        ])
        let helpMenu = submenu("Help", [item("Controls", #selector(showControls(_:)), "?")])
        app.mainMenu = main
        app.windowsMenu = windowMenu
        app.helpMenu = helpMenu
    }
}

/// The view options, kept in the user defaults.
struct ViewSettings {
    var layout = ScreenLayout()
    var smoothing = false

    private static let arrangementKey = "arrangement", integerKey = "integerScaling", smoothKey = "smoothing"

    static func load() -> ViewSettings {
        let d = UserDefaults.standard
        var s = ViewSettings()
        if let raw = d.string(forKey: arrangementKey), let a = ScreenLayout.Arrangement(rawValue: raw) { s.layout.arrangement = a }
        s.layout.integerScaling = d.bool(forKey: integerKey)
        s.smoothing = d.bool(forKey: smoothKey)
        return s
    }

    func save() {
        let d = UserDefaults.standard
        d.set(layout.arrangement.rawValue, forKey: Self.arrangementKey)
        d.set(layout.integerScaling, forKey: Self.integerKey)
        d.set(smoothing, forKey: Self.smoothKey)
    }
}
