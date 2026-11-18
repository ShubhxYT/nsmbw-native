import AppKit
import CoreGraphics
import Foundation

let size = 1024
let space = CGColorSpaceCreateDeviceRGB()
guard let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
                          bytesPerRow: 0, space: space,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
    exit(1)
}

let full = CGRect(x: 0, y: 0, width: size, height: size)

// Background: deep navy to blue, top-left light.
let colors = [
    CGColor(red: 0.05, green: 0.09, blue: 0.22, alpha: 1),
    CGColor(red: 0.10, green: 0.24, blue: 0.52, alpha: 1),
] as CFArray
if let gradient = CGGradient(colorsSpace: space, colors: colors, locations: [0, 1]) {
    ctx.drawLinearGradient(gradient,
                           start: CGPoint(x: 0, y: CGFloat(size)),
                           end: CGPoint(x: CGFloat(size), y: 0),
                           options: [])
}

// Warm ring behind the monogram.
ctx.setStrokeColor(CGColor(red: 0.96, green: 0.72, blue: 0.24, alpha: 0.95))
ctx.setLineWidth(26)
ctx.strokeEllipse(in: CGRect(x: 214, y: 214, width: 596, height: 596))

// Soft inner disc.
ctx.setFillColor(CGColor(red: 0.07, green: 0.13, blue: 0.30, alpha: 0.75))
ctx.fillEllipse(in: CGRect(x: 250, y: 250, width: 524, height: 524))

// The monogram.
let font = NSFont.systemFont(ofSize: 470, weight: .heavy)
let attributes: [NSAttributedString.Key: Any] = [
    .font: font,
    .foregroundColor: NSColor.white,
]
let text = NSAttributedString(string: "N", attributes: attributes)
let line = CTLineCreateWithAttributedString(text)
let bounds = CTLineGetBoundsWithOptions(line, .useOpticalBounds)

ctx.textPosition = CGPoint(x: (CGFloat(size) - bounds.width) / 2 - bounds.minX,
                           y: (CGFloat(size) - bounds.height) / 2 - bounds.minY)
CTLineDraw(line, ctx)

guard let image = ctx.makeImage() else { exit(1) }
let rep = NSBitmapImageRep(cgImage: image)
guard let data = rep.representation(using: .png, properties: [:]) else { exit(1) }
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon-1024.png"
try data.write(to: URL(fileURLWithPath: out))
print(out)
