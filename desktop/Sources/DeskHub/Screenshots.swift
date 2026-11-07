// `DeskHub --screenshots <dir> [--package <zip>]`: the App draws its own menu and every settings page into PNG files (for the
// README), with the data it really has, and quits. Nothing is changed: it only reads (the USB status of the touch screen
// included).
import AppKit
import SwiftUI

@MainActor
enum Screenshots {
    static func runIfAsked(model: AppModel, firmware: FirmwareModel) {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: "--screenshots"), i + 1 < a.count else { return }
        let dir = URL(fileURLWithPath: a[i + 1])
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        firmware.refreshPorts()
        firmware.loadKey()
        if let j = a.firstIndex(of: "--package"), j + 1 < a.count { firmware.load(URL(fileURLWithPath: a[j + 1])) }
        // Time for the display, the read-back and the Bluetooth link to come in. The USB status is not read: opening the port
        // can restart the touch screen.
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) {
            NSApp.appearance = NSAppearance(named: .darkAqua)
            firmware.refreshPorts()
            let heights: [SettingsPage: CGFloat] = [.displays: 470, .inputs: 400, .device: 260, .firmware: 560, .general: 240]
            for p in SettingsPage.allCases {
                let view = SettingsView(page: p).environmentObject(model).environmentObject(model.link).environmentObject(firmware)
                write(view, width: 900, height: heights[p] ?? 600, to: dir.appendingPathComponent("settings-\(file(p)).png"))
            }
            write(MenuView().environmentObject(model).environmentObject(model.link), width: 340, height: nil, to: dir.appendingPathComponent("menu.png"))
            exit(0)
        }
    }

    static func file(_ p: SettingsPage) -> String {
        switch p {
        case .displays: return "displays"
        case .inputs: return "inputs"
        case .device: return "device"
        case .firmware: return "firmware"
        case .general: return "general"
        }
    }

    // height nil: as tall as the view wants at this width.
    static func write<V: View>(_ view: V, width: CGFloat, height: CGFloat?, to url: URL) {
        // Off screen there is no window background: draw one, and keep the dark look the README shows.
        let host = NSHostingView(rootView: view.frame(width: width).background(Color(nsColor: NSColor(white: 0.13, alpha: 1))).environment(\.colorScheme, .dark))
        let h = height ?? max(100, host.fittingSize.height)
        host.frame = CGRect(x: 0, y: 0, width: width, height: h)
        let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: .darkAqua)
        window.contentView = host
        // Let the views appear first (onAppear fills the text fields), then draw.
        RunLoop.current.run(until: Date().addingTimeInterval(0.6))
        host.layoutSubtreeIfNeeded()
        host.display()
        guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return }
        host.cacheDisplay(in: host.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: url)
    }
}
