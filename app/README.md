# The Apple app

One DS game, recompiled to C, in a native app for the Mac, iPhone and iPad, and Apple TV. A
Swift shell (`app/`) around the C runtime (`runtime/`): Metal draws the two screens, the
emulation runs on a thread of its own at the DS's 59.8261 Hz, AVAudioEngine plays the sound,
and the keyboard, game controllers, the mouse and touch are the buttons and the stylus.

Each app is built **around one game**: `tools/build_app.py` recompiles your ROM, builds the
game and the runtime as a static library (`libdsn.a`) for the platform, builds the app against
it, and puts the ROM next to the executable. Build it locally; the result holds your ROM, so
never commit it or pass it on.

| Path | What |
|---|---|
| `Sources/DSNCore` | C shim: the runtime and the game behind a small interface (`dsn_core.h`); Swift never sees `Nds` |
| `Sources/DSNKit` | the shared code: the emulation loop and pacing, the Metal renderer, layouts, input, audio, the iOS and tvOS views |
| `Sources/DSNative` | the Mac app (AppKit) |
| `Sources/DSNativeiOS`, `Sources/DSNativeTV` | the iPhone/iPad app (SwiftUI) and the Apple TV app (UIKit) |
| `Support/Info-*.plist` | each app's Info.plist (build settings fill in the game's names) |
| `Package.swift` | the package: DSNKit, and the Mac executable for `swift build` |
| `project.yml` | the Xcode project, for [XcodeGen](https://github.com/yonaskolb/XcodeGen): macOS, iOS and tvOS targets sharing DSNKit |
| `Tests/` | `swift test`: layouts, input, the pad's geometry, the Metal renderer, the paced emulation loop |

## Build

You need a Mac with Xcode (the macOS, iOS and tvOS SDKs, `xcodebuild`, and for the shaders the
Metal Toolchain component), Python 3.10 or later with `capstone` 5 (the recompiler), and for the
iOS and tvOS apps XcodeGen (`brew install xcodegen`). The Mac app alone builds with the Command
Line Tools. Apple silicon: `libdsn.a` is arm64.

```
python3 tools/build_app.py game.nds -o build/game-app                       # the Mac app
python3 tools/build_app.py game.nds -o build/game-app --platform ios-sim    # iOS Simulator
python3 tools/build_app.py game.nds -o build/game-app --platform tvos-sim   # Apple TV Simulator
python3 tools/build_app.py game.nds -o build/game-app --platform ios        # iPhone and iPad, unsigned
python3 tools/build_app.py game.nds -o build/game-app --platform tvos       # Apple TV, unsigned
```

All platforms can share one output directory: the recompiler's output is shared (and replaced
only where it changed, so a rerun compiles nothing again) and each platform's objects and
library go in `game-<platform>`. The app lands in `OUTDIR/<name>.app` (Mac) or
`OUTDIR/<name>-<platform>.app`.

| Option | |
|---|---|
| `--name NAME` | the app's name (default: the ROM's title) |
| `--bundle-id ID` | default `com.dsnative.game.<game code>`; the save is kept by it, and a device needs one that is yours |
| `--save-type TYPE` | the cartridge's save chip (`flash512k`, `eeprom8k`, ...) when the runtime can't tell from the game code; kept in the Info.plist |
| `--extra missing.txt` | entry points the recompiler missed (the runtime's `missing.txt`) |
| `--jobs N` | parallel C compiles (default 4) |
| `--opt O0\|O1\|O2\|Os` | optimization of the generated C (default O2) |
| `--xcode` | build the Mac app through the Xcode project instead of SwiftPM |
| `--run [--device NAME] [--screenshot PNG]` | simulator platforms: boot a device, install, launch, wait, screenshot, terminate |

Nothing is signed (`CODE_SIGNING_ALLOWED=NO`) and no keychain is touched. The Mac app gets an
ad hoc signature, which Apple silicon needs to run it at all. The simulators run unsigned apps.

For a quick look without a window, the Mac app has a smoke mode:

```
build/game-app/Name.app/Contents/MacOS/DSNative --smoke 100 --png out.png
```

(That is the default SwiftPM build. With `--xcode` the executable is named after the app, and
both builds write `OUTDIR/<name>.app`, so one replaces the other.)

It runs 100 frames through the same frame loop (`--keys` and `--touch` take `dsn_headless`'s
input scripts, other `--name value` pairs are runtime options such as `--save-type`) and writes
both screens as a 256x384 PNG. `tests/test_app.py` uses it to compare the app's pixels with
`dsn_headless`'s.

## Controls

| Mac keyboard | DS |
|---|---|
| Arrow keys | D-pad |
| X / Z | A / B |
| S / A | X / Y |
| Q / W | L / R |
| Return, keypad Enter | Start |
| Right Shift | Select |
| Mouse, trackpad | the stylus, on the bottom screen |

The same keys work on a hardware keyboard on an iPad or Apple TV.

**Game controllers** (GameController.framework, on every platform): the face buttons by
position, as on the DS (right A, bottom B, top X, left Y), shoulders and triggers are L and R,
Menu is Start, Options is Select, the D-pad or the left stick steer.

**iPhone and iPad**: an on-screen pad (D-pad, A B X Y, L R, Select, Start) steps aside while a
controller is in use (connected, and it has sent input); a finger on the bottom screen is the stylus. Portrait stacks the
screens above the pad; landscape on a phone stacks them between the pad's two halves, on an iPad
puts them side by side. The "..." button has pause, the landscape arrangement, the pad, integer
scaling and smoothing.

**Apple TV**: both screens side by side, as large as the system's overscan margin allows.
A controller works as above. The Siri Remote is a minimal pad: its touch surface is the D-pad,
the click is A, Play/Pause is Start.

**Mac menus**: Emulation (Pause, Step Frame, show the save in Finder), View (stacked or side by
side, integer scaling, smoothing, window sizes, full screen). The choices are remembered.

## Saves, sound, pausing

- The save file is `<game code>.sav`: in the Mac app's Application Support folder
  (`~/Library/Application Support/<bundle id>/`), on iPhone and iPad in the app's Documents
  (the Files app shows it: copy a save out or in), on Apple TV in Application Support, which
  the system may clear when the Apple TV runs short of space. It is written every 10 seconds
  when the game changed it, when the app leaves the foreground, and on quit.
- Sound is the runtime's mixer (`nds_audio_take`, 32768 Hz stereo) through an `AVAudioEngine`
  source node. It plays silence while the runtime has no sound. On iPhone and iPad the audio
  session is `ambient` (it follows the silent switch and mixes with other audio), on Apple TV
  `playback`. The engine restarts after an interruption (a call, Siri) and after a change of
  output device.
- The game pauses (and goes quiet) while the app is not in front, or from the menu.

## Running on a device

`build_app.py` builds unsigned, which is enough for the simulators. A device needs a signature
from you:

1. Build for the device once, so `libdsn.a` for that SDK exists:
   `python3 tools/build_app.py game.nds -o build/game-app --platform ios` (or `tvos`), with a
   `--bundle-id` of your own (for example `com.yourname.dsnative.pokemon`): a bundle identifier
   can belong to only one team.
2. Open `build/game-app/xcode/DSNative.xcodeproj` in Xcode, pick the `DSNative-iOS` or
   `DSNative-tvOS` scheme and your device, and under Signing & Capabilities choose your team. A
   free Apple ID works (Xcode makes a development certificate and profile for it; such apps
   expire after a week and a free account may sign only a few at a time). The project reads its
   settings from `build/game-app/Game.xcconfig`.
3. The iPhone or iPad needs Developer Mode (Settings > Privacy & Security). An Apple TV is paired
   in Xcode (Window > Devices and Simulators).
4. Run. Nothing is generated at run time, so the app does not need a debugger attached or any
   JIT entitlement.

## Size and build time: the test ROM, and big games

The test ROM (`python3 tests/hw/build.py build/hwtest`, 33 KB) is nearly a best case:

| | |
|---|---|
| generated C | 19,565 lines in 6 files, 688 KB |
| `libdsn.a` | 346 KB (Mac and simulators), 344 KB (devices) |
| recompile | 0.2 s |
| clean `--platform ios-sim` build (recompile, library, XcodeGen, Xcode with the Swift package) | 11 s |
| the `.app` | 1.0 MB (executable 986 KB; the ROM and `arm9.bin` 50 KB) |

A commercial game is not like that. For a game like Pokémon Black 2 the generated C is about
300 MB and the library about 100 MB (the project's figures for that game; they were not
measured again for this app, and they depend on how much code discovery finds). Expect:

- a long C compile per platform at `-O2` (not timed for Black 2 here): raise `--jobs` on an idle
  machine, or use `--opt O1` or `O0` for a first look (the library is then slower to run);
- the same again for every platform, since each has a library of its own;
- 100 MB or more of library linked into the executable, and the whole ROM (up to 512 MB) in the
  bundle, which the runtime reads into memory at start;
- a simulator or device build that spends most of its time in `make`, not in Xcode: the Swift
  part is a few seconds either way.

## Tests

```
python3 tests/test_app.py        # builds the Mac app on the test ROM, runs it (--smoke), swift test
swift test --package-path app -Xlinker -L<OUT>/game-macos      # with DSN_TEST_ROM and DSN_TEST_ARM9 set, as test_app.py does
```

The package's tests also run in a simulator, where the iOS-only ones (does a finger outside the
pad's controls reach the screen under it?) run too, and the renderer runs on the simulator's
Metal device. Build a simulator library first (`--platform ios-sim`), then:

```
cd app && xcodebuild test -scheme DSNKitTests -destination 'platform=iOS Simulator,name=iPhone 17' \
    -derivedDataPath ../build/dd-test ARCHS=arm64 ONLY_ACTIVE_ARCH=YES LIBRARY_SEARCH_PATHS=<OUT>/game-ios-sim
```

`test_app.py` skips with a message where Swift isn't installed. It compares the app's two
screens with `dsn_headless`'s pixel for pixel, checks the save file and the bundle, checks that
the deployment targets in `build_app.py`, `Package.swift` and `project.yml` agree, and has
XcodeGen make the project. It does not build for iOS or tvOS or use a simulator: that check is
`build_app.py --platform ios-sim --run` (and `tvos-sim`), whose screenshot should show the test
ROM's Mandelbrot above and the tiled background with the diamond below.

## Limits

- One game per process: the dispatcher's caches are static.
- Sound is silent until the runtime's mixer lands; the audio ring then rides out short stalls
  (125 ms), but the emulation is paced by the host clock and the audio device by its own, so a
  drop-out every few minutes is possible until the two are tied together.
- Not in the app: the microphone, closing the lid, Wi-Fi.
- The iPhone and iPad touch handling was looked at in the simulators and its hit testing is
  tested (a finger outside the pad's controls reaches the screen below it), but it was not
  driven with real taps: `simctl` can't send touches.
