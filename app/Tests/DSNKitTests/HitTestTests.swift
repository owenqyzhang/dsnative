#if os(iOS)
import SwiftUI
import Testing
import UIKit
@testable import DSNKit

/// What a finger reaches in the iPhone screen's view stack: the pad's controls take it, and
/// anywhere else it goes on to the view under the pad (the Metal view with the touch screen).
/// That depends on how SwiftUI hosts the pad's UIView, so it is checked on a real UIKit window:
/// these tests only exist for iOS, and run in a simulator:
///
///     xcodebuild test -scheme DSNKit -destination 'platform=iOS Simulator,name=iPhone 17' \
///         -derivedDataPath build/dd-test LIBRARY_SEARCH_PATHS=<OUT>/game-ios-sim
@MainActor
struct HitTestTests {
    /// Stands in for the Metal view under the pad.
    final class Probe: UIView {}

    struct ProbeHost: UIViewRepresentable {
        func makeUIView(context: Context) -> Probe { Probe() }
        func updateUIView(_ view: Probe, context: Context) {}
    }

    func window(size: CGSize, geometry: PadGeometry) -> UIWindow {
        let content = ZStack(alignment: .topLeading) {
            ProbeHost()
            PadOverlay(geometry: geometry, onChange: { _ in }).frame(width: size.width, height: size.height)
        }
        .frame(width: size.width, height: size.height)
        let controller = UIHostingController(rootView: content)
        let window = UIWindow(frame: CGRect(origin: .zero, size: size))
        window.rootViewController = controller
        window.isHidden = false
        controller.view.frame = window.bounds
        controller.view.layoutIfNeeded()
        return window
    }

    @Test func controlsTakeTouchesAndTheRestGoesToTheScreenBelow() throws {
        let size = CGSize(width: 402, height: 874)
        let layout = PlayfieldLayout.make(size: size, safe: Insets(top: 62, bottom: 34))
        let geometry = try #require(layout.pad)
        let win = window(size: size, geometry: geometry)

        // on the A button
        let a = try #require(geometry.controls.first { $0.kind == .button(.a, label: "A") })
        guard case let .circle(centre, _) = a.shape else { Issue.record("A is not a circle"); return }
        #expect(win.hitTest(centre, with: nil) is PadView)

        // on the bottom screen, well away from every control
        let onScreen = CGPoint(x: layout.screens.midX, y: layout.screens.midY + layout.screens.height / 4)
        #expect(!geometry.contains(onScreen))
        #expect(win.hitTest(onScreen, with: nil) is Probe)
    }
}
#endif
