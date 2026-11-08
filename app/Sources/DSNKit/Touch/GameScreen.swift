#if os(iOS)
import SwiftUI

/// The iPhone and iPad screen: the game, the on-screen pad and a menu. See `PlayfieldLayout`
/// for where each goes.
public struct GameScreen: View {
    @Bindable var model: GameModel
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingControls = false

    public init(model: GameModel) {
        self.model = model
    }

    public var body: some View {
        // This reader keeps the safe area, which is the only way to read its insets (a view that
        // ignores the safe area is told they are zero). The playfield is sized to the whole window
        // and moved back out under the notch and the home indicator.
        GeometryReader { outer in
            let safe = outer.safeAreaInsets
            let size = CGSize(width: outer.size.width + safe.leading + safe.trailing,
                              height: outer.size.height + safe.top + safe.bottom)
            playfield(size: size, insets: Insets(top: safe.top, left: safe.leading, bottom: safe.bottom, right: safe.trailing))
                .frame(width: size.width, height: size.height)
                .offset(x: -safe.leading, y: -safe.top)
        }
        .background(Color.black.ignoresSafeArea())
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .task { model.start() }
        .onChange(of: scenePhase) { _, phase in model.setForeground(phase == .active) }
        .alert("Controls", isPresented: $showingControls) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("""
            On-screen buttons, or a game controller, or a keyboard (arrows, X A, Z B, S X, A Y, Q L, W R, Return Start, \
            right Shift Select). Touch the bottom screen to use the stylus.
            """)
        }
    }

    private func playfield(size: CGSize, insets: Insets) -> some View {
        let layout = PlayfieldLayout.make(size: size, safe: insets, options: shown(model.layoutOptions), style: .touch)
        return ZStack(alignment: .topLeading) {
            Color.black
            switch model.phase {
            case .loading:
                ProgressView().tint(.white).frame(maxWidth: .infinity, maxHeight: .infinity)
            case let .failed(message):
                Text(message).foregroundStyle(.white).multilineTextAlignment(.center).padding().frame(maxWidth: .infinity, maxHeight: .infinity)
            case .playing:
                if let emulator = model.session?.emulator {
                    ScreenHost(emulator: emulator,
                               layout: ScreenLayout(arrangement: layout.arrangement, integerScaling: model.layoutOptions.integerScaling),
                               smoothing: model.smoothing, onFrame: { model.displayTick() },
                               onTouch: { model.touchChanged($0) })
                        .frame(width: layout.screens.width, height: layout.screens.height)
                        .position(x: layout.screens.midX, y: layout.screens.midY)
                }
                if let pad = layout.pad {
                    PadOverlay(geometry: pad, onChange: { model.padChanged($0) })
                        .frame(width: size.width, height: size.height)
                }
                if model.userPaused {
                    Text("Paused").font(.title2.weight(.semibold)).foregroundStyle(.white)
                        .padding(.horizontal, 20).padding(.vertical, 10)
                        .background(.black.opacity(0.6), in: Capsule())
                        .position(x: layout.screens.midX, y: layout.screens.midY)
                        .allowsHitTesting(false)
                }
                if let stopped = model.stopped {
                    Text(stopped).font(.footnote).foregroundStyle(.red)
                        .position(x: size.width / 2, y: max(insets.top, 12) + 4)
                        .allowsHitTesting(false)
                }
                menu.position(layout.menu ?? CGPoint(x: size.width / 2, y: 30))
            }
        }
        .frame(width: size.width, height: size.height)
    }

    /// The options with the pad off while a controller is connected.
    private func shown(_ o: PlayfieldLayout.Options) -> PlayfieldLayout.Options {
        var o = o
        o.showPad = model.showsPad
        return o
    }

    private var menu: some View {
        Menu {
            Button(model.userPaused ? "Resume" : "Pause", systemImage: model.userPaused ? "play.fill" : "pause.fill") {
                model.togglePause()
            }
            Picker("In landscape", selection: $model.layoutOptions.landscape) {
                Text("Automatic").tag(ScreenLayout.Arrangement?.none)
                Text("Stacked").tag(ScreenLayout.Arrangement?.some(.stacked))
                Text("Side by side").tag(ScreenLayout.Arrangement?.some(.sideBySide))
            }
            Toggle("On-screen controls", isOn: $model.layoutOptions.showPad)
            Toggle("Integer scaling", isOn: $model.layoutOptions.integerScaling)
            Toggle("Smooth pixels", isOn: $model.smoothing)
            Button("Controls", systemImage: "gamecontroller") { showingControls = true }
        } label: {
            Image(systemName: "ellipsis.circle.fill")
                .font(.system(size: 26))
                .foregroundStyle(.white.opacity(0.55))
                .frame(width: 44, height: 44)
                .contentShape(Rectangle())
        }
    }
}
#endif
