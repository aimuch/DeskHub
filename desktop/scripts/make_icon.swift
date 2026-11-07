// Draws the App icon (docs/design/app, "夜航" look): a dark rounded square, a cyan monitor, two arrows that swap.
//   swift desktop/scripts/make_icon.swift <out.iconset>   then   iconutil -c icns <out.iconset>
import AppKit

let out = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
let cyan = NSColor(srgbRed: 0x2f / 255, green: 0xd8 / 255, blue: 0xff / 255, alpha: 1)

func draw(_ px: Int) -> Data {
    let s = CGFloat(px)
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let k = s / 1024
    // the macOS icon grid: a 824 square with rounded corners, inside 1024
    let tile = NSRect(x: 100 * k, y: 100 * k, width: 824 * k, height: 824 * k)
    let shape = NSBezierPath(roundedRect: tile, xRadius: 185 * k, yRadius: 185 * k)
    NSGradient(starting: NSColor(srgbRed: 0.07, green: 0.10, blue: 0.13, alpha: 1), ending: NSColor(srgbRed: 0.01, green: 0.02, blue: 0.03, alpha: 1))!
        .draw(in: shape, angle: -90)
    NSColor(srgbRed: 0.18, green: 0.25, blue: 0.30, alpha: 1).setStroke()
    shape.lineWidth = max(1, 4 * k)
    shape.stroke()
    // the monitor
    cyan.setStroke()
    let screen = NSBezierPath(roundedRect: NSRect(x: 250 * k, y: 390 * k, width: 524 * k, height: 340 * k), xRadius: 44 * k, yRadius: 44 * k)
    screen.lineWidth = 42 * k
    screen.stroke()
    let stand = NSBezierPath()
    stand.move(to: NSPoint(x: 512 * k, y: 390 * k)); stand.line(to: NSPoint(x: 512 * k, y: 300 * k))
    stand.move(to: NSPoint(x: 400 * k, y: 290 * k)); stand.line(to: NSPoint(x: 624 * k, y: 290 * k))
    stand.lineWidth = 42 * k
    stand.lineCapStyle = .round
    stand.stroke()
    // two arrows that swap, inside the screen
    let arrows = NSBezierPath()
    arrows.move(to: NSPoint(x: 360 * k, y: 600 * k)); arrows.line(to: NSPoint(x: 660 * k, y: 600 * k))
    arrows.move(to: NSPoint(x: 600 * k, y: 650 * k)); arrows.line(to: NSPoint(x: 660 * k, y: 600 * k)); arrows.line(to: NSPoint(x: 600 * k, y: 550 * k))
    arrows.move(to: NSPoint(x: 664 * k, y: 515 * k)); arrows.line(to: NSPoint(x: 364 * k, y: 515 * k))
    arrows.move(to: NSPoint(x: 424 * k, y: 565 * k)); arrows.line(to: NSPoint(x: 364 * k, y: 515 * k)); arrows.line(to: NSPoint(x: 424 * k, y: 465 * k))
    arrows.lineWidth = 34 * k
    arrows.lineCapStyle = .round
    arrows.lineJoinStyle = .round
    arrows.stroke()
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for (name, px) in [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128), ("128x128@2x", 256),
                   ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)] {
    try! draw(px).write(to: out.appendingPathComponent("icon_\(name).png"))
}
