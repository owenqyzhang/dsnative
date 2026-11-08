import DSNKit
import UIKit

/// The Apple TV app: one game, the one tools/build_app.py bundled into it, on a view controller
/// that takes the remote and controllers as game input (TVGameViewController in DSNKit).
@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    static let model = GameModel()

    func applicationWillTerminate(_ application: UIApplication) {
        Self.model.shutDown()
    }
}

/// Named for the Info.plist (UISceneDelegateClassName), whatever the app is called.
@objc(SceneDelegate)
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let window = UIWindow(windowScene: windowScene)
        window.rootViewController = TVGameViewController(model: AppDelegate.model)
        window.makeKeyAndVisible()
        self.window = window
    }

    func sceneDidBecomeActive(_ scene: UIScene) { AppDelegate.model.setForeground(true) }
    func sceneWillResignActive(_ scene: UIScene) { AppDelegate.model.setForeground(false) }
}
