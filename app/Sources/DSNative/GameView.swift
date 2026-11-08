import AppKit
import DSNKit
import MetalKit

/// The window's content: the Metal view the screens are drawn in, taking the keyboard (see
/// KeyboardMap) and the mouse or trackpad as the stylus on the bottom screen.
@MainActor
final class GameView: MTKView {
    var emulator: Emulator?
    /// The layout the renderer draws with, for mapping the pointer to the touch screen.
    var currentLayout: () -> ScreenLayout = { ScreenLayout() }

    private var heldKeys: Set<UInt16> = []
    private var touching = false
    private static let rightShift: UInt16 = 0x3C

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    // MARK: keyboard

    override func keyDown(with event: NSEvent) {
        // Command shortcuts belong to the menus; every other key is swallowed (no beep)
        if event.modifierFlags.contains(.command) { return super.keyDown(with: event) }
        if KeyboardMap.macKeyCodes[event.keyCode] != nil, heldKeys.insert(event.keyCode).inserted { publishKeys() }
    }

    override func keyUp(with event: NSEvent) {
        if heldKeys.remove(event.keyCode) != nil { publishKeys() }
    }

    override func flagsChanged(with event: NSEvent) {
        if event.keyCode == Self.rightShift {
            // right Shift has its own device-dependent flag (NX_DEVICERSHIFTKEYMASK)
            if event.modifierFlags.rawValue & 0x4 != 0 { heldKeys.insert(Self.rightShift) } else { heldKeys.remove(Self.rightShift) }
            publishKeys()
        }
        super.flagsChanged(with: event)
    }

    /// Lets go of everything, when the window stops getting events (no stuck buttons).
    func releaseAll() {
        heldKeys.removeAll()
        publishKeys()
        if touching {
            touching = false
            emulator?.setTouch(.up)
        }
    }

    override func resignFirstResponder() -> Bool {
        releaseAll()
        return super.resignFirstResponder()
    }

    private func publishKeys() {
        let held = heldKeys.reduce(into: DSButtons()) { $0.formUnion(KeyboardMap.macKeyCodes[$1] ?? []) }
        emulator?.setButtons(held, from: .keyboard)
    }

    // MARK: mouse and trackpad

    override func mouseDown(with event: NSEvent) {
        guard let p = touchPoint(event, clamp: false) else { return }
        touching = true
        emulator?.setTouch(.at(p.x, p.y))
    }

    override func mouseDragged(with event: NSEvent) {
        guard touching, let p = touchPoint(event, clamp: true) else { return }
        emulator?.setTouch(.at(p.x, p.y))
    }

    override func mouseUp(with event: NSEvent) {
        guard touching else { return }
        touching = false
        emulator?.setTouch(.up)
    }

    /// The event's position in drawable pixels from the top left, through the layout.
    private func touchPoint(_ event: NSEvent, clamp: Bool) -> (x: Int, y: Int)? {
        let local = convert(event.locationInWindow, from: nil)
        let size = drawableSize
        let sx = size.width / max(bounds.width, 1), sy = size.height / max(bounds.height, 1)
        let y = isFlipped ? local.y : bounds.height - local.y
        return currentLayout().touchPoint(at: CGPoint(x: local.x * sx, y: y * sy), in: size, clamp: clamp)
    }
}
