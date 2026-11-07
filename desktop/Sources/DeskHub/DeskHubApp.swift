// DeskHub: a menu bar App that switches the monitor's input over DDC (docs/desktop-app-design.md section 13).
import AppKit
import SwiftUI

// A menu bar App (LSUIElement) is never the active App, so its windows do not become key: clicks get lost, text fields
// lose focus, pages lag. While the settings window is open DeskHub is a regular App (with a Dock icon) and comes to the
// front; when the window closes it goes back to the menu bar only.
enum SettingsWindow {
    static let title = "DeskHub 设置"

    @MainActor static func open(_ openWindow: OpenWindowAction) {
        NSApp.setActivationPolicy(.regular)
        openWindow(id: "settings")
        NSApp.activate(ignoringOtherApps: true)
        DispatchQueue.main.async {
            NSApp.windows.first { $0.title == title }?.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: nil, queue: .main) { note in
            guard (note.object as? NSWindow)?.title == SettingsWindow.title else { return }
            DispatchQueue.main.async { NSApp.setActivationPolicy(.accessory) }
        }
    }

    // Only one DeskHub at a time: a second one brings the first forward and quits (two would send two commands).
    func applicationWillFinishLaunching(_ notification: Notification) {
        guard let id = Bundle.main.bundleIdentifier else { return }
        let others = NSRunningApplication.runningApplications(withBundleIdentifier: id).filter { $0 != NSRunningApplication.current }
        if let first = others.first {
            first.activate()
            exit(0)
        }
    }
}

@main
struct DeskHubApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        MenuBarExtra {
            MenuView().environmentObject(model)
        } label: {
            Image(systemName: menuIcon)
        }
        .menuBarExtraStyle(.window)

        Window("DeskHub 设置", id: "settings") {
            SettingsView().environmentObject(model)
        }
        .windowResizability(.contentSize)
    }

    // Normal, switching, or something wrong (not bound, display missing, DDC not there).
    private var menuIcon: String {
        if model.busyID != nil { return "arrow.triangle.2.circlepath" }
        if model.ddcProblem != nil { return "display.trianglebadge.exclamationmark" }
        if case .found = model.match { return "display" }
        return "display.trianglebadge.exclamationmark"
    }
}
