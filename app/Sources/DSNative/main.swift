import AppKit
import DSNKit

let options: LaunchOptions
do {
    options = try LaunchOptions.parse(CommandLine.arguments)
} catch {
    printError("DSNative: \(error)\n\n\(LaunchOptions.usage)")
    exit(2)
}
if options.help {
    print(LaunchOptions.usage)
    exit(0)
}
if let frames = options.smokeFrames {
    exit(Smoke.run(options, frames: frames))
}

let app = NSApplication.shared
let delegate = AppDelegate(options: options)
app.delegate = delegate
app.setActivationPolicy(.regular)     // also when run outside a bundle (swift run)
app.run()
