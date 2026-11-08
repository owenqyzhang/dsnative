#if os(tvOS)
import GameController
import MetalKit
import UIKit

/// The Apple TV screen: both screens large inside the overscan margin, taking the Siri Remote
/// and game controllers as game input rather than as focus movement.
public final class TVGameViewController: GCEventViewController {
    private let model: GameModel
    private let metal = GameMetalView(frame: .zero, device: MTLCreateSystemDefaultDevice())
    private var renderer: ScreenRenderer?
    private let status = UILabel()

    public init(model: GameModel) {
        self.model = model
        super.init(nibName: nil, bundle: nil)
        // controller events are the game's: no focus engine, no Menu button going back
        controllerUserInteractionEnabled = false
    }

    required init?(coder: NSCoder) { fatalError("not from a nib") }

    public override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black
        view.addSubview(metal)
        status.textColor = .white
        status.font = .systemFont(ofSize: 36, weight: .semibold)
        status.textAlignment = .center
        view.addSubview(status)
        model.start()
        switch model.phase {
        case .playing:
            if let emulator = model.session?.emulator {
                renderer = try? ScreenRenderer(view: metal, emulator: emulator)
                renderer?.onDisplayFrame = { [model] in model.displayTick() }
            }
        case let .failed(message):
            status.text = message
            status.numberOfLines = 0
        case .loading:
            break
        }
    }

    public override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        // the safe area is the margin tvOS keeps for overscan (about 90 pt at the sides, 60 above and below)
        let i = view.safeAreaInsets
        let layout = PlayfieldLayout.make(size: view.bounds.size, safe: Insets(top: i.top, left: i.left, bottom: i.bottom, right: i.right),
                                          options: model.layoutOptions, style: .tv)
        metal.frame = layout.screens
        status.frame = view.bounds.inset(by: i)
        renderer?.layout = ScreenLayout(arrangement: layout.arrangement, integerScaling: model.layoutOptions.integerScaling)
        renderer?.smoothing = model.smoothing
    }
}
#endif
