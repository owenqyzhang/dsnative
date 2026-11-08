import GameController

/// Game controllers through GameController.framework, polled once per display refresh.
///
/// Face buttons map by position, as on the DS: the right button is A, the bottom B, the top X
/// and the left Y (GameController names them by the Xbox layout: B, A, Y, X). Shoulders and
/// triggers are L and R, Menu is Start, Options is Select; the d-pad and left stick steer.
/// The Siri Remote (a micro gamepad) is a minimal pad: its touch surface is the d-pad, the click
/// is A and Play/Pause is Start. Hardware keyboards on iPad and Apple TV follow `KeyboardMap`.
@MainActor
public final class GameControllers {
    nonisolated(unsafe) private var observers: [NSObjectProtocol] = []
    /// The controllers that have sent input since they connected (see `hasGamepad`).
    private var used: Set<ObjectIdentifier> = []

    public init() {
        // a Siri Remote's touch surface as a d-pad: where the thumb rests is the direction
        // (relative values would need a swipe), and the remote's orientation is not rotated
        for controller in GCController.controllers() { Self.configure(controller) }
        observers.append(NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { for controller in GCController.controllers() { Self.configure(controller) } }
        })
        observers.append(NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] note in
            let id = (note.object as? GCController).map(ObjectIdentifier.init)
            MainActor.assumeIsolated { if let id { self?.used.remove(id) } }
        })
    }

    deinit {
        for o in observers { NotificationCenter.default.removeObserver(o) }
    }

    private static func configure(_ controller: GCController) {
        controller.microGamepad?.reportsAbsoluteDpadValues = true
        controller.microGamepad?.allowsRotation = false
    }

    /// For a help screen.
    public static let summary = """
    Controllers: face buttons by position (right A, bottom B, top X, left Y), shoulders and \
    triggers L and R, Menu Start, Options Select, d-pad or left stick to move. \
    Siri Remote: touch surface d-pad, click A, Play/Pause Start.
    """

    /// Whether a controller (not the Siri Remote, which is always there on Apple TV) is in use:
    /// connected and having sent input since. A controller that is only paired, or one the iOS
    /// Simulator lists without anyone holding it, doesn't take the on-screen pad away.
    public var hasGamepad: Bool {
        GCController.controllers().contains { $0.extendedGamepad != nil && used.contains(ObjectIdentifier($0)) }
    }

    /// The buttons held on every connected controller, and on a hardware keyboard.
    public func poll() -> DSButtons {
        var held: DSButtons = []
        for controller in GCController.controllers() {
            if let pad = controller.extendedGamepad {
                let now = Self.buttons(of: pad)
                if !now.isEmpty { used.insert(ObjectIdentifier(controller)) }
                held.formUnion(now)
            } else if let pad = controller.microGamepad {
                held.formUnion(Self.buttons(of: pad))
            }
        }
        #if os(iOS) || os(tvOS)
        if let keyboard = GCKeyboard.coalesced?.keyboardInput {
            for (usage, button) in KeyboardMap.hidUsages where keyboard.button(forKeyCode: GCKeyCode(rawValue: CFIndex(usage)))?.isPressed == true {
                held.formUnion(button)
            }
        }
        #endif
        return held
    }

    static func buttons(of pad: GCExtendedGamepad) -> DSButtons {
        var held: DSButtons = []
        if pad.buttonB.isPressed { held.insert(.a) }
        if pad.buttonA.isPressed { held.insert(.b) }
        if pad.buttonY.isPressed { held.insert(.x) }
        if pad.buttonX.isPressed { held.insert(.y) }
        if pad.leftShoulder.isPressed || pad.leftTrigger.isPressed { held.insert(.l) }
        if pad.rightShoulder.isPressed || pad.rightTrigger.isPressed { held.insert(.r) }
        if pad.buttonMenu.isPressed { held.insert(.start) }
        if pad.buttonOptions?.isPressed == true { held.insert(.select) }
        held.formUnion(directions(pad.dpad))
        held.formUnion(directions(pad.leftThumbstick))
        return held
    }

    static func buttons(of pad: GCMicroGamepad) -> DSButtons {
        var held = directions(pad.dpad)
        if pad.buttonA.isPressed { held.insert(.a) }          // the click
        if pad.buttonX.isPressed { held.insert(.start) }      // Play/Pause
        return held
    }

    /// A d-pad or stick; a stick counts past half way.
    static func directions(_ pad: GCControllerDirectionPad) -> DSButtons {
        var held: DSButtons = []
        if pad.up.isPressed || pad.yAxis.value > 0.5 { held.insert(.up) }
        if pad.down.isPressed || pad.yAxis.value < -0.5 { held.insert(.down) }
        if pad.left.isPressed || pad.xAxis.value < -0.5 { held.insert(.left) }
        if pad.right.isPressed || pad.xAxis.value > 0.5 { held.insert(.right) }
        return held
    }
}
