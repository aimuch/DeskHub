// swift-tools-version:5.9
// The DeskHub desktop App (docs/desktop-app-design.md section 13): a menu bar App that switches the monitor's input over DDC.
// No Xcode needed: `swift build`, then scripts/make_app.sh puts DeskHub.app together.
import PackageDescription

let package = Package(
    name: "DeskHub",
    platforms: [.macOS(.v13)],
    targets: [
        // The rules: configuration, binding, white list, switch results (K2, K3). No AppKit, no IOKit: tested on its own.
        .target(name: "DeskHubCore"),
        // DDC over IOAVService (private API, Apple silicon only). The only code that talks to a monitor.
        .target(name: "DDC", dependencies: ["DeskHubCore"]),
        // USB flashing of the touch screen (docs/desktop-app-design.md 13.10): the package on disk, esptool, the USB status.
        .target(name: "Flasher", dependencies: ["DeskHubCore"]),
        // The menu bar App.
        .executableTarget(name: "DeskHub", dependencies: ["DeskHubCore", "DDC", "Flasher"]),
        // USB flashing from the command line, through the same code as the App (the end-to-end run, the evidence).
        .executableTarget(name: "deskhub-flash", dependencies: ["DeskHubCore", "Flasher"]),
        // A command line for the experiments and the evidence: list, read, and switch through the same code as the App.
        .executableTarget(name: "deskhub-ddc", dependencies: ["DeskHubCore", "DDC"]),
        // The tests of DeskHubCore. A plain program, not `swift test`: the command line tools have no Testing macros.
        // `swift run core-check` exits 1 when a check fails.
        .executableTarget(name: "core-check", dependencies: ["DeskHubCore"]),
    ]
)
