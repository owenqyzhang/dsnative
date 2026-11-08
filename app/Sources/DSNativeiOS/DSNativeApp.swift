import DSNKit
import SwiftUI

/// The iPhone and iPad app: one game, the one tools/build_app.py bundled into it. The screen,
/// the on-screen pad and the rules for pausing are in DSNKit (GameScreen, GameModel).
@main
struct DSNativeApp: App {
    @State private var model = GameModel()

    var body: some Scene {
        WindowGroup {
            GameScreen(model: model)
                .preferredColorScheme(.dark)
        }
    }
}
