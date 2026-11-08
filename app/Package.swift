// swift-tools-version: 6.0
// The dsnative app: DSNKit (the game loop, Metal screens, input, audio; shared by every Apple
// platform) and the macOS app around it. Linking needs a game: libdsn.a, the runtime and a
// recompiled game, from `make -C OUT libdsn.a`. Pass its directory with -Xlinker -L<OUT>, or
// let tools/build_app.py do it all.
import PackageDescription

let package = Package(
    name: "DSNative",
    platforms: [.macOS(.v14), .iOS(.v17), .tvOS(.v17)],
    products: [
        .library(name: "DSNKit", targets: ["DSNKit"]),
        .executable(name: "DSNative", targets: ["DSNative"]),
    ],
    targets: [
        // C glue between Swift and the runtime (Swift never sees Nds itself)
        .target(name: "DSNCore", linkerSettings: [.linkedLibrary("dsn")]),
        .target(name: "DSNKit", dependencies: ["DSNCore"],
                // the screen shader's source, as a byte array in the module (see ScreenRenderer)
                resources: [.embedInCode("Shaders/screen.metal")]),
        .executableTarget(name: "DSNative", dependencies: ["DSNKit"]),
        .testTarget(name: "DSNKitTests", dependencies: ["DSNKit"]),
    ]
)
