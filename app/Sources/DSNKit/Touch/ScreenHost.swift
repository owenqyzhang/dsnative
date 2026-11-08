#if os(iOS)
import MetalKit
import SwiftUI

/// The Metal view with the screens, in SwiftUI. Its frame is the rectangle the layout gives the
/// screens; the renderer arranges the two inside it.
struct ScreenHost: UIViewRepresentable {
    let emulator: Emulator
    var layout: ScreenLayout
    var smoothing: Bool
    var onFrame: () -> Void
    var onTouch: (TouchState) -> Void

    @MainActor
    final class Coordinator {
        var renderer: ScreenRenderer?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> GameMetalView {
        let view = GameMetalView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.isOpaque = true
        context.coordinator.renderer = try? ScreenRenderer(view: view, emulator: emulator)
        return view
    }

    func updateUIView(_ view: GameMetalView, context: Context) {
        guard let renderer = context.coordinator.renderer else { return }
        renderer.layout = layout
        renderer.smoothing = smoothing
        renderer.onDisplayFrame = onFrame
        view.layout = { [weak renderer] in renderer?.layout ?? layout }
        view.onTouch = onTouch
    }
}
#endif
