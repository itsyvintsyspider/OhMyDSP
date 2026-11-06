// Renders the pane icon (rounded square, microphone symbol) as a 1024 px PNG: swift makeicon.swift out.png
import AppKit
let size: CGFloat = 1024
let img = NSImage(size: NSSize(width: size, height: size))
img.lockFocus()
let rect = NSRect(x: 40, y: 40, width: size - 80, height: size - 80)
let path = NSBezierPath(roundedRect: rect, xRadius: 190, yRadius: 190)
NSGradient(colors: [NSColor(calibratedRed: 0.36, green: 0.66, blue: 1.0, alpha: 1), NSColor(calibratedRed: 0.0, green: 0.35, blue: 0.9, alpha: 1)])!.draw(in: path, angle: -90)
if let sym = NSImage(systemSymbolName: "mic.fill", accessibilityDescription: nil)?.withSymbolConfiguration(.init(pointSize: 520, weight: .regular)) {
    let tinted = NSImage(size: sym.size, flipped: false) { r in sym.draw(in: r); NSColor.white.set(); r.fill(using: .sourceAtop); return true }
    tinted.draw(in: NSRect(x: (size - tinted.size.width) / 2, y: (size - tinted.size.height) / 2, width: tinted.size.width, height: tinted.size.height))
}
img.unlockFocus()
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
