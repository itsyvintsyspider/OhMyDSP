// Renders the OhMyDSP icon as a 1024 px PNG: swift makeicon.swift out.png
// A DSP chip with a sound waveform on it, circled by an orbit ring that passes behind and in front of it, on a blue squircle.
import AppKit

let S: CGFloat = 1024
func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> NSColor { NSColor(calibratedRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: a) }
func rounded(_ r: NSRect, _ radius: CGFloat) -> NSBezierPath { NSBezierPath(roundedRect: r, xRadius: radius, yRadius: radius) }

let img = NSImage(size: NSSize(width: S, height: S))
img.lockFocus()
let ctx = NSGraphicsContext.current!.cgContext
let body = NSRect(x: 40, y: 40, width: S - 80, height: S - 80)
let squircle = rounded(body, 190)

// background: blue gradient with a soft gloss on the upper half
NSGraphicsContext.saveGraphicsState()
squircle.addClip()
NSGradient(colors: [rgb(112, 190, 255), rgb(22, 104, 232), rgb(8, 58, 200)], atLocations: [0, 0.55, 1], colorSpace: .deviceRGB)!.draw(in: body, angle: -90)
NSGradient(colors: [NSColor.white.withAlphaComponent(0.30), NSColor.white.withAlphaComponent(0)])!
    .draw(in: NSBezierPath(ovalIn: NSRect(x: -140, y: 520, width: S + 280, height: 760)), angle: -90)
NSGraphicsContext.restoreGraphicsState()

// orbit ring (tilted ellipse). `back` draws the whole ring; `front` re-draws only its lower half over the chip.
func orbit(frontOnly: Bool) {
    NSGraphicsContext.saveGraphicsState()
    let t = NSAffineTransform(); t.translateX(by: S / 2, yBy: S / 2 - 6); t.rotate(byDegrees: -24); t.concat()
    if frontOnly { NSBezierPath(rect: NSRect(x: -700, y: -700, width: 1400, height: 700)).addClip() }
    let ring = NSBezierPath(ovalIn: NSRect(x: -400, y: -150, width: 800, height: 300)); ring.lineWidth = 26
    let sh = NSShadow(); sh.shadowColor = rgb(0, 30, 120, 0.45); sh.shadowBlurRadius = 14; sh.shadowOffset = NSSize(width: 0, height: -8); sh.set()
    rgb(240, 248, 255, 0.95).setStroke(); ring.stroke()
    NSGraphicsContext.restoreGraphicsState()
}
orbit(frontOnly: false)

// chip: pins, body, waveform
let chip = NSRect(x: S / 2 - 190, y: S / 2 - 190, width: 380, height: 380)
NSGraphicsContext.saveGraphicsState()
let pinGrad = NSGradient(colors: [rgb(236, 240, 246), rgb(150, 160, 176)])!
for i in 0..<5 {
    let o = CGFloat(i) * 70 + 20
    for r in [NSRect(x: chip.minX + o + 8, y: chip.maxY - 6, width: 34, height: 62), NSRect(x: chip.minX + o + 8, y: chip.minY - 56, width: 34, height: 62),
              NSRect(x: chip.minX - 56, y: chip.minY + o + 8, width: 62, height: 34), NSRect(x: chip.maxX - 6, y: chip.minY + o + 8, width: 62, height: 34)] {
        pinGrad.draw(in: rounded(r, 9), angle: 0)
    }
}
let drop = NSShadow(); drop.shadowColor = rgb(0, 20, 90, 0.55); drop.shadowBlurRadius = 30; drop.shadowOffset = NSSize(width: 0, height: -16); drop.set()
let cp = rounded(chip, 64)
rgb(20, 24, 32).setFill(); cp.fill()
NSGraphicsContext.restoreGraphicsState()
NSGradient(colors: [rgb(70, 78, 94), rgb(22, 26, 34)])!.draw(in: cp, angle: -90)
rgb(255, 255, 255, 0.18).setStroke(); let edge = rounded(chip.insetBy(dx: 3, dy: 3), 61); edge.lineWidth = 3; edge.stroke()
// pin-1 dot
rgb(150, 215, 255).setFill(); NSBezierPath(ovalIn: NSRect(x: chip.minX + 26, y: chip.maxY - 46, width: 16, height: 16)).fill()

let bars: [CGFloat] = [70, 160, 105, 245, 150, 200, 85]
let w: CGFloat = 26, gap: CGFloat = 16
let total = CGFloat(bars.count) * w + CGFloat(bars.count - 1) * gap
for (i, h) in bars.enumerated() {
    let r = NSRect(x: S / 2 - total / 2 + CGFloat(i) * (w + gap), y: S / 2 - h / 2, width: w, height: h)
    NSGraphicsContext.saveGraphicsState()
    let glow = NSShadow(); glow.shadowColor = rgb(90, 210, 255, 0.9); glow.shadowBlurRadius = 18; glow.shadowOffset = .zero; glow.set()
    NSGradient(colors: [rgb(255, 255, 255), rgb(110, 225, 255)])!.draw(in: rounded(r, w / 2), angle: -90)
    NSGraphicsContext.restoreGraphicsState()
}

orbit(frontOnly: true)
img.unlockFocus()
_ = ctx
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
