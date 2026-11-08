import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

public enum PNGWriter {
    public struct WriteError: Error, CustomStringConvertible {
        public let description: String
    }

    /// Writes 0xAARRGGBB pixels (the runtime's framebuffer format) as an opaque PNG.
    public static func write(_ pixels: UnsafePointer<UInt32>, width: Int, height: Int, to url: URL) throws {
        let data = Data(bytes: pixels, count: width * height * 4)
        // little-endian 0xAARRGGBB words are B, G, R, A in memory: 32-bit little, alpha first
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let provider = CGDataProvider(data: data as CFData),
              let space = CGColorSpace(name: CGColorSpace.sRGB),
              let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: width * 4, space: space, bitmapInfo: info, provider: provider,
                                  decode: nil, shouldInterpolate: false, intent: .defaultIntent),
              let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)
        else { throw WriteError(description: "cannot write \(url.path)") }
        CGImageDestinationAddImage(dest, image, nil)
        guard CGImageDestinationFinalize(dest) else { throw WriteError(description: "cannot write \(url.path)") }
    }
}
