// Renders Droidmount app icon candidates (1024 px PNG) from SF Symbols.
//   swift scripts/icon.swift <out-dir> [variant]   variant: a | b | c | all (default)
// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

let size: CGFloat = 1024
let body = NSRect(x: 100, y: 100, width: 824, height: 824)   // macOS icon grid
let radius: CGFloat = 186

func symbol(_ name: String, pointSize: CGFloat, weight: NSFont.Weight, color: NSColor) -> NSImage? {
    let cfg = NSImage.SymbolConfiguration(pointSize: pointSize, weight: weight)
        .applying(.init(paletteColors: [color]))
    return NSImage(systemSymbolName: name, accessibilityDescription: nil)?
        .withSymbolConfiguration(cfg)
}

func drawCentered(_ img: NSImage, center: NSPoint) {
    let r = NSRect(x: center.x - img.size.width / 2, y: center.y - img.size.height / 2,
                   width: img.size.width, height: img.size.height)
    img.draw(in: r)
}

func squircle(_ top: NSColor, _ bottom: NSColor) {
    let path = NSBezierPath(roundedRect: body, xRadius: radius, yRadius: radius)
    NSGradient(starting: top, ending: bottom)!.draw(in: path, angle: -90)
    // soft highlight at the top, like the system icons
    NSGraphicsContext.saveGraphicsState()
    path.addClip()
    let gloss = NSBezierPath(ovalIn: NSRect(x: body.minX - 200, y: body.midY + 40,
                                            width: body.width + 400, height: body.height))
    NSColor.white.withAlphaComponent(0.10).setFill()
    gloss.fill()
    NSGraphicsContext.restoreGraphicsState()
}

/// A modern Android phone: rounded body, edge-to-edge screen, centred punch-hole camera
/// (the punch hole is what tells it apart from an iPhone). Returns the screen rect.
func phone(center: NSPoint, width: CGFloat, height: CGFloat, frame: NSColor, screen: NSColor) -> NSRect {
    let body = NSRect(x: center.x - width / 2, y: center.y - height / 2, width: width, height: height)
    frame.setFill()
    NSBezierPath(roundedRect: body, xRadius: width * 0.16, yRadius: width * 0.16).fill()
    let bezel = width * 0.055
    let scr = body.insetBy(dx: bezel, dy: bezel)
    // no shadow inside the phone
    NSGraphicsContext.saveGraphicsState()
    NSShadow().set()
    screen.setFill()
    NSBezierPath(roundedRect: scr, xRadius: width * 0.12, yRadius: width * 0.12).fill()
    let cam = width * 0.07
    frame.blended(withFraction: 0.35, of: .black)?.setFill()
    NSBezierPath(ovalIn: NSRect(x: scr.midX - cam / 2, y: scr.maxY - cam * 1.8, width: cam, height: cam)).fill()
    NSGraphicsContext.restoreGraphicsState()
    return scr
}

func render(_ variant: String) -> NSImage {
    let img = NSImage(size: NSSize(width: size, height: size))
    img.lockFocus()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.25)
    shadow.shadowOffset = NSSize(width: 0, height: -12)
    shadow.shadowBlurRadius = 28

    let androidGreen = NSColor(srgbRed: 0.24, green: 0.86, blue: 0.52, alpha: 1)

    switch variant {
    case "a":
        // Android green; white phone whose screen shows a folder: "your phone is a disk"
        squircle(NSColor(srgbRed: 0.55, green: 0.88, blue: 0.48, alpha: 1),
                 NSColor(srgbRed: 0.12, green: 0.55, blue: 0.30, alpha: 1))
        NSGraphicsContext.saveGraphicsState()
        shadow.set()
        let screen = phone(center: NSPoint(x: 512, y: 512), width: 380, height: 660,
                           frame: .white, screen: NSColor(srgbRed: 0.09, green: 0.22, blue: 0.14, alpha: 1))
        NSGraphicsContext.restoreGraphicsState()
        if let folder = symbol("folder.fill", pointSize: 170, weight: .regular, color: androidGreen) {
            drawCentered(folder, center: NSPoint(x: screen.midX, y: screen.midY))
        }
    case "b":
        // Finder-like blue with a folder; Android phone in front of it
        squircle(NSColor(srgbRed: 0.40, green: 0.75, blue: 1.00, alpha: 1),
                 NSColor(srgbRed: 0.10, green: 0.42, blue: 0.86, alpha: 1))
        if let folder = symbol("folder.fill", pointSize: 440, weight: .regular,
                               color: NSColor.white.withAlphaComponent(0.92)) {
            drawCentered(folder, center: NSPoint(x: 450, y: 500))
        }
        NSGraphicsContext.saveGraphicsState()
        shadow.set()
        _ = phone(center: NSPoint(x: 680, y: 400), width: 250, height: 440,
                  frame: NSColor(srgbRed: 0.13, green: 0.15, blue: 0.17, alpha: 1), screen: androidGreen)
        NSGraphicsContext.restoreGraphicsState()
    default:
        // dark and minimal: Android phone plugged into a USB-C cable
        squircle(NSColor(srgbRed: 0.22, green: 0.24, blue: 0.27, alpha: 1),
                 NSColor(srgbRed: 0.07, green: 0.08, blue: 0.09, alpha: 1))
        // cable first, so the phone covers its top end
        NSColor.white.setFill()
        NSBezierPath(roundedRect: NSRect(x: 492, y: 120, width: 40, height: 140), xRadius: 8, yRadius: 8).fill()
        NSBezierPath(roundedRect: NSRect(x: 470, y: 230, width: 84, height: 80), xRadius: 18, yRadius: 18).fill()
        NSGraphicsContext.saveGraphicsState()
        shadow.set()
        let screen = phone(center: NSPoint(x: 512, y: 590), width: 330, height: 560,
                           frame: NSColor(srgbRed: 0.85, green: 0.87, blue: 0.89, alpha: 1), screen: androidGreen)
        NSGraphicsContext.restoreGraphicsState()
        if let arrows = symbol("arrow.up.arrow.down", pointSize: 150, weight: .bold,
                               color: NSColor(srgbRed: 0.05, green: 0.25, blue: 0.13, alpha: 1)) {
            drawCentered(arrows, center: NSPoint(x: screen.midX, y: screen.midY - 10))
        }
    }
    img.unlockFocus()
    return img
}

func savePNG(_ img: NSImage, _ url: URL) {
    guard let tiff = img.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else { return }
    try? png.write(to: url)
}

// The Android robot is reproduced or modified from work created and shared by Google and
// used according to terms described in the Creative Commons 3.0 Attribution License.
let droidGreen = NSColor(srgbRed: 0x3D / 255.0, green: 0xDC / 255.0, blue: 0x84 / 255.0, alpha: 1)

/// Android robot head: half disc, two antennas, two eyes. `base` is the middle of its
/// flat bottom edge.
func droidHead(base: NSPoint, width w: CGFloat, color: NSColor, eyes: NSColor) {
    let r = w / 2
    let head = NSBezierPath()
    head.move(to: NSPoint(x: base.x - r, y: base.y))
    head.appendArc(withCenter: base, radius: r, startAngle: 180, endAngle: 0, clockwise: true)
    head.close()
    color.setFill()
    head.fill()
    // antennas, rounded strokes leaning outwards
    let antenna = NSBezierPath()
    antenna.lineWidth = w * 0.045
    antenna.lineCapStyle = .round
    for side: CGFloat in [-1, 1] {
        let a0 = NSPoint(x: base.x + side * r * 0.42, y: base.y + r * 0.86)
        let a1 = NSPoint(x: base.x + side * r * 0.66, y: base.y + r * 1.22)
        antenna.move(to: a0)
        antenna.line(to: a1)
    }
    color.setStroke()
    antenna.stroke()
    eyes.setFill()
    let e = w * 0.085
    for side: CGFloat in [-1, 1] {
        NSBezierPath(ovalIn: NSRect(x: base.x + side * r * 0.42 - e / 2, y: base.y + r * 0.42,
                                    width: e, height: e)).fill()
    }
}

func renderDroid(_ variant: String) -> NSImage {
    let img = NSImage(size: NSSize(width: size, height: size))
    img.lockFocus()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.28)
    shadow.shadowOffset = NSSize(width: 0, height: -12)
    shadow.shadowBlurRadius = 26
    switch variant {
    case "d":
        // dark squircle, the robot sitting on a white external drive
        squircle(NSColor(srgbRed: 0.20, green: 0.22, blue: 0.25, alpha: 1),
                 NSColor(srgbRed: 0.06, green: 0.07, blue: 0.08, alpha: 1))
        NSGraphicsContext.saveGraphicsState()
        shadow.set()
        let drive = NSRect(x: 222, y: 250, width: 580, height: 230)
        NSColor(white: 0.95, alpha: 1).setFill()
        NSBezierPath(roundedRect: drive, xRadius: 60, yRadius: 60).fill()
        NSGraphicsContext.restoreGraphicsState()
        droidGreen.setFill()
        NSBezierPath(ovalIn: NSRect(x: 690, y: 345, width: 40, height: 40)).fill() // activity light
        NSColor(white: 0.80, alpha: 1).setFill()
        NSBezierPath(roundedRect: NSRect(x: 290, y: 352, width: 300, height: 26), xRadius: 13, yRadius: 13).fill()
        droidHead(base: NSPoint(x: 512, y: 500), width: 400, color: droidGreen, eyes: NSColor(white: 0.1, alpha: 1))
    case "e":
        // Finder blue: the robot peeking over a folder
        squircle(NSColor(srgbRed: 0.40, green: 0.75, blue: 1.00, alpha: 1),
                 NSColor(srgbRed: 0.10, green: 0.42, blue: 0.86, alpha: 1))
        droidHead(base: NSPoint(x: 512, y: 520), width: 420, color: droidGreen, eyes: .white)
        NSGraphicsContext.saveGraphicsState()
        shadow.set()
        if let folder = symbol("folder.fill", pointSize: 470, weight: .regular,
                               color: NSColor(white: 0.97, alpha: 1)) {
            drawCentered(folder, center: NSPoint(x: 512, y: 380))
        }
        NSGraphicsContext.restoreGraphicsState()
    default:
        // Android green squircle, white robot on a USB-C plug
        squircle(NSColor(srgbRed: 0.45, green: 0.90, blue: 0.60, alpha: 1),
                 NSColor(srgbRed: 0.10, green: 0.62, blue: 0.36, alpha: 1))
        NSGraphicsContext.saveGraphicsState()
        shadow.set()
        NSColor.white.setFill()
        NSBezierPath(roundedRect: NSRect(x: 412, y: 300, width: 200, height: 190), xRadius: 40, yRadius: 40).fill()
        NSBezierPath(roundedRect: NSRect(x: 462, y: 170, width: 100, height: 150), xRadius: 22, yRadius: 22).fill()
        droidHead(base: NSPoint(x: 512, y: 520), width: 420, color: .white,
                  eyes: NSColor(srgbRed: 0.10, green: 0.55, blue: 0.32, alpha: 1))
        NSGraphicsContext.restoreGraphicsState()
        NSColor(srgbRed: 0.10, green: 0.55, blue: 0.32, alpha: 1).setFill()
        NSBezierPath(roundedRect: NSRect(x: 482, y: 200, width: 60, height: 18), xRadius: 9, yRadius: 9).fill()
    }
    img.unlockFocus()
    return img
}

let args = CommandLine.arguments
let out = URL(fileURLWithPath: args.count > 1 ? args[1] : ".")
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
let which = args.count > 2 ? args[2] : "droid"
let variants = which == "all" ? ["a", "b", "c"] : which == "droid" ? ["d", "e", "f"] : [which]
var rendered: [NSImage] = []
for v in variants {
    let img = ["d", "e", "f"].contains(v) ? renderDroid(v) : render(v)
    rendered.append(img)
    savePNG(img, out.appendingPathComponent("icon-\(v).png"))
}

// side-by-side preview with labels
if rendered.count > 1 {
    let tile: CGFloat = 512, pad: CGFloat = 48
    let sheet = NSImage(size: NSSize(width: CGFloat(rendered.count) * (tile + pad) + pad, height: tile + 160))
    sheet.lockFocus()
    NSColor(white: 0.93, alpha: 1).setFill()
    NSRect(origin: .zero, size: sheet.size).fill()
    for (i, img) in rendered.enumerated() {
        let x = pad + CGFloat(i) * (tile + pad)
        img.draw(in: NSRect(x: x, y: 110, width: tile, height: tile))
        let label = variants[i].uppercased() as NSString
        label.draw(at: NSPoint(x: x + tile / 2 - 20, y: 30),
                   withAttributes: [.font: NSFont.boldSystemFont(ofSize: 64), .foregroundColor: NSColor.darkGray])
    }
    sheet.unlockFocus()
    savePNG(sheet, out.appendingPathComponent("icon-candidates-\(which).png"))
}
