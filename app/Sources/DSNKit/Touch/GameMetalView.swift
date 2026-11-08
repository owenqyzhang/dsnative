#if os(iOS) || os(tvOS)
import MetalKit
import UIKit

/// The Metal view the screens are drawn in. On iOS a finger on the bottom screen is the stylus.
public final class GameMetalView: MTKView {
    /// The layout the renderer draws with (to find the touch screen under a finger).
    var layout: () -> ScreenLayout = { ScreenLayout() }
    var onTouch: (TouchState) -> Void = { _ in }
    #if os(iOS)
    private var stylus: UITouch?

    public override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        // the first finger on the bottom screen; others are ignored until it lifts
        guard stylus == nil else { return }
        for t in touches {
            if let p = screenPoint(t, clamp: false) {
                stylus = t
                onTouch(.at(p.x, p.y))
                return
            }
        }
    }

    public override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        // a drag that leaves the screen keeps its edge pixel, as on the hardware
        if let t = stylus, touches.contains(t), let p = screenPoint(t, clamp: true) { onTouch(.at(p.x, p.y)) }
    }

    public override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }
    public override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { lift(touches) }

    private func lift(_ touches: Set<UITouch>) {
        if let t = stylus, touches.contains(t) {
            stylus = nil
            onTouch(.up)
        }
    }

    /// The DS touch-screen pixel under the touch: points to drawable pixels, then through the layout.
    private func screenPoint(_ t: UITouch, clamp: Bool) -> (x: Int, y: Int)? {
        let p = t.location(in: self)
        let sx = drawableSize.width / max(bounds.width, 1), sy = drawableSize.height / max(bounds.height, 1)
        return layout().touchPoint(at: CGPoint(x: p.x * sx, y: p.y * sy), in: drawableSize, clamp: clamp)
    }
    #endif
}
#endif
