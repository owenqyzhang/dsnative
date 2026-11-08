import Metal
import MetalKit
import QuartzCore

/// Draws both screens with Metal: each display refresh it uploads the emulator's newest frame
/// into a pair of textures and draws two quads where the layout puts the screens.
///
/// The shaders (Shaders/screen.metal) come from the app's precompiled default.metallib when it
/// has them (the Xcode project compiles it; tools/build_app.py does for the Mac app), and are
/// compiled from the source embedded in this module otherwise (swift test, swift run).
@MainActor
public final class ScreenRenderer: NSObject, MTKViewDelegate {
    public var layout = ScreenLayout()
    /// Linear filtering instead of sharp pixels.
    public var smoothing = false
    /// Called at the start of each display refresh, before drawing (controller polling).
    public var onDisplayFrame: (() -> Void)?

    public let device: MTLDevice
    public let pixelFormat: MTLPixelFormat
    /// Whether the shaders came from a precompiled library (false: compiled at run time).
    public let usesPrecompiledShaders: Bool
    private var emulator: Emulator?
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let nearest: MTLSamplerState
    private let linear: MTLSamplerState
    /// One pair (top, bottom) per frame in flight, so an upload never overwrites a texture
    /// the GPU is still reading.
    private let textures: [(MTLTexture, MTLTexture)]
    private var next = 0
    private let inFlight: DispatchSemaphore

    public init(device: MTLDevice, pixelFormat: MTLPixelFormat = .bgra8Unorm) throws {
        guard let queue = device.makeCommandQueue() else { throw RenderError("no Metal command queue") }
        self.device = device
        self.pixelFormat = pixelFormat
        self.queue = queue

        let library: MTLLibrary
        if let precompiled = device.makeDefaultLibrary(), precompiled.functionNames.contains("screen_vertex"),
           precompiled.functionNames.contains("screen_fragment") {
            library = precompiled
            usesPrecompiledShaders = true
        } else {
            library = try device.makeLibrary(source: String(decoding: PackageResources.screen_metal, as: UTF8.self),
                                             options: nil)
            usesPrecompiledShaders = false
        }
        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "screen_vertex")
        desc.fragmentFunction = library.makeFunction(name: "screen_fragment")
        desc.colorAttachments[0].pixelFormat = pixelFormat
        pipeline = try device.makeRenderPipelineState(descriptor: desc)

        func sampler(_ filter: MTLSamplerMinMagFilter) throws -> MTLSamplerState {
            let d = MTLSamplerDescriptor()
            d.minFilter = filter
            d.magFilter = filter
            d.sAddressMode = .clampToEdge
            d.tAddressMode = .clampToEdge
            guard let s = device.makeSamplerState(descriptor: d) else { throw RenderError("no sampler") }
            return s
        }
        nearest = try sampler(.nearest)
        linear = try sampler(.linear)

        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: Core.screenWidth,
                                                          height: Core.screenHeight, mipmapped: false)
        td.usage = .shaderRead
        let frames = 3
        var made: [(MTLTexture, MTLTexture)] = []
        for _ in 0..<frames {
            guard let top = device.makeTexture(descriptor: td), let bottom = device.makeTexture(descriptor: td) else {
                throw RenderError("no texture")
            }
            made.append((top, bottom))
        }
        textures = made
        inFlight = DispatchSemaphore(value: frames)
        super.init()
    }

    /// A renderer that draws `emulator`'s frames into `view` at each display refresh.
    public convenience init(view: MTKView, emulator: Emulator) throws {
        guard let device = view.device ?? MTLCreateSystemDefaultDevice() else { throw RenderError("no Metal device") }
        try self.init(device: device, pixelFormat: .bgra8Unorm)
        self.emulator = emulator
        view.device = device
        view.colorPixelFormat = pixelFormat
        view.framebufferOnly = true
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.preferredFramesPerSecond = 60
        #if os(macOS)
        // the framebuffer is sRGB; don't let the display stretch it to its own gamut
        (view.layer as? CAMetalLayer)?.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        #endif
        view.delegate = self
    }

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    public func draw(in view: MTKView) {
        onDisplayFrame?()
        guard let emulator, let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let commands = queue.makeCommandBuffer() else { return }
        encode(emulator.latestFrame(), into: commands, pass: pass, size: view.drawableSize)
        commands.present(drawable)
        commands.commit()
    }

    /// Draws `pixels` (both screens, `Core.pixelCount` words) as the view would at `size`, and
    /// returns the picture as 0xAARRGGBB words, row by row from the top. For tests.
    public func render(_ pixels: UnsafePointer<UInt32>, width: Int, height: Int) throws -> [UInt32] {
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: pixelFormat, width: width, height: height, mipmapped: false)
        td.usage = .renderTarget
        td.storageMode = .private
        let rowBytes = width * 4
        guard let target = device.makeTexture(descriptor: td),
              let readback = device.makeBuffer(length: rowBytes * height, options: .storageModeShared),
              let commands = queue.makeCommandBuffer() else { throw RenderError("no offscreen target") }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        encode(pixels, into: commands, pass: pass, size: CGSize(width: width, height: height))
        // a private texture can't be read by the CPU; copy it out (works on every GPU)
        guard let blit = commands.makeBlitCommandEncoder() else { throw RenderError("no blit encoder") }
        blit.copy(from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: width, height: height, depth: 1), to: readback, destinationOffset: 0,
                  destinationBytesPerRow: rowBytes, destinationBytesPerImage: rowBytes * height)
        blit.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        if let error = commands.error { throw error }
        let words = readback.contents().bindMemory(to: UInt32.self, capacity: width * height)
        return Array(UnsafeBufferPointer(start: words, count: width * height))
    }

    /// Uploads both screens and draws them where the layout puts them in a target of `size`.
    private func encode(_ frame: UnsafePointer<UInt32>, into commands: MTLCommandBuffer, pass: MTLRenderPassDescriptor,
                        size: CGSize) {
        inFlight.wait()
        let done = inFlight
        commands.addCompletedHandler { _ in done.signal() }
        let (top, bottom) = textures[next]
        next = (next + 1) % textures.count
        let w = Core.screenWidth, h = Core.screenHeight
        let region = MTLRegionMake2D(0, 0, w, h)
        top.replace(region: region, mipmapLevel: 0, withBytes: frame, bytesPerRow: w * 4)
        bottom.replace(region: region, mipmapLevel: 0, withBytes: frame + w * h, bytesPerRow: w * 4)

        guard let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return }
        encoder.setRenderPipelineState(pipeline)
        encoder.setFragmentSamplerState(smoothing ? linear : nearest, index: 0)
        let rects = layout.frames(in: size)
        for (texture, rect) in [(top, rects.top), (bottom, rects.bottom)] {
            var quad = Self.quad(rect, in: size)
            encoder.setVertexBytes(&quad, length: MemoryLayout<Quad>.stride, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        encoder.endEncoding()
    }

    /// A screen's rectangle (pixels, origin top left) in normalized device coordinates.
    struct Quad {
        var corners: SIMD4<Float>   // x0, y0 (top left), x1, y1 (bottom right)
    }

    static func quad(_ r: CGRect, in size: CGSize) -> Quad {
        let w = max(size.width, 1), h = max(size.height, 1)
        return Quad(corners: SIMD4(Float(r.minX / w * 2 - 1), Float(1 - r.minY / h * 2),
                                   Float(r.maxX / w * 2 - 1), Float(1 - r.maxY / h * 2)))
    }
}

struct RenderError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}
