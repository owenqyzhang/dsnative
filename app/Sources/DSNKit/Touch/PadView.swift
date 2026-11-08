#if os(iOS)
import SwiftUI
import UIKit

/// Draws a `PadGeometry` and reads fingers on it. It covers the whole window but only answers
/// for the controls (`point(inside:)`): everywhere else the touch goes through to the screens.
final class PadView: UIView {
    var geometry: PadGeometry? {
        didSet { if geometry != oldValue { setNeedsDisplay() } }
    }
    var onChange: (DSButtons) -> Void = { _ in }
    private var fingers: Set<UITouch> = []
    private var held: DSButtons = []
    private let haptic = UIImpactFeedbackGenerator(style: .light)

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        isOpaque = false
        backgroundColor = .clear
        contentMode = .redraw
    }

    required init?(coder: NSCoder) { fatalError("not from a nib") }

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        geometry?.contains(point) ?? false
    }

    // MARK: touches

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { fingers.formUnion(touches); update() }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { update() }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { fingers.subtract(touches); update() }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { fingers.subtract(touches); update() }

    private func update() {
        let now = geometry?.buttons(at: fingers.map { $0.location(in: self) }) ?? []
        guard now != held else { return }
        if !now.subtracting(held).isEmpty { haptic.impactOccurred() }
        held = now
        onChange(now)
        setNeedsDisplay()
    }

    /// Lets go of everything (the app left the foreground, or the pad went away).
    func releaseAll() {
        fingers.removeAll()
        update()
    }

    // MARK: drawing

    private static let ink = UIColor.white

    override func draw(_ rect: CGRect) {
        guard let geometry else { return }
        for control in geometry.controls {
            switch (control.kind, control.shape) {
            case let (.dpad, .circle(center, radius)):
                drawCross(center: center, radius: radius)
            case let (.button(button, label), .circle(center, radius)):
                let path = UIBezierPath(arcCenter: center, radius: radius, startAngle: 0, endAngle: 2 * .pi, clockwise: true)
                paint(path, pressed: held.contains(button))
                text(label, in: CGRect(x: center.x - radius, y: center.y - radius, width: 2 * radius, height: 2 * radius),
                     size: radius * 0.8)
            case let (.button(button, label), .pill(r)):
                let path = UIBezierPath(roundedRect: r, cornerRadius: r.height / 2)
                paint(path, pressed: held.contains(button))
                text(label, in: r, size: r.height * (label.count > 1 ? 0.36 : 0.5))
            default:
                break
            }
        }
    }

    private func paint(_ path: UIBezierPath, pressed: Bool) {
        Self.ink.withAlphaComponent(pressed ? 0.55 : 0.18).setFill()
        path.fill()
        Self.ink.withAlphaComponent(0.55).setStroke()
        path.lineWidth = 1.5
        path.stroke()
    }

    private func text(_ s: String, in rect: CGRect, size: CGFloat) {
        let font = UIFont.systemFont(ofSize: size, weight: .semibold)
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: Self.ink.withAlphaComponent(0.85)]
        let sz = (s as NSString).size(withAttributes: attrs)
        (s as NSString).draw(at: CGPoint(x: rect.midX - sz.width / 2, y: rect.midY - sz.height / 2), withAttributes: attrs)
    }

    /// A plus: a round-ended bar each way, and the arms for the held directions filled in.
    private func drawCross(center c: CGPoint, radius r: CGFloat) {
        let arm = r * 0.62, len = r * 2 * 0.95
        let horizontal = CGRect(x: c.x - len / 2, y: c.y - arm / 2, width: len, height: arm)
        let vertical = CGRect(x: c.x - arm / 2, y: c.y - len / 2, width: arm, height: len)
        let path = UIBezierPath(roundedRect: horizontal, cornerRadius: arm * 0.3)
        path.append(UIBezierPath(roundedRect: vertical, cornerRadius: arm * 0.3))
        paint(path, pressed: false)
        let tip = (len - arm) / 2
        let arms: [(DSButtons, CGRect)] = [
            (.left, CGRect(x: horizontal.minX, y: horizontal.minY, width: tip + arm * 0.5, height: arm)),
            (.right, CGRect(x: horizontal.maxX - tip - arm * 0.5, y: horizontal.minY, width: tip + arm * 0.5, height: arm)),
            (.up, CGRect(x: vertical.minX, y: vertical.minY, width: arm, height: tip + arm * 0.5)),
            (.down, CGRect(x: vertical.minX, y: vertical.maxY - tip - arm * 0.5, width: arm, height: tip + arm * 0.5)),
        ]
        for (button, rect) in arms where held.contains(button) {
            Self.ink.withAlphaComponent(0.5).setFill()
            UIBezierPath(roundedRect: rect, cornerRadius: arm * 0.3).fill()
        }
        // the triangles pointing out
        Self.ink.withAlphaComponent(0.7).setFill()
        let t = arm * 0.22, o = len / 2 - arm * 0.3
        for (dx, dy) in [(1.0, 0.0), (-1.0, 0.0), (0.0, 1.0), (0.0, -1.0)] {
            let tipPoint = CGPoint(x: c.x + dx * o, y: c.y + dy * o)
            let back = CGPoint(x: tipPoint.x - dx * t, y: tipPoint.y - dy * t)
            let tri = UIBezierPath()
            tri.move(to: tipPoint)
            tri.addLine(to: CGPoint(x: back.x - dy * t, y: back.y + dx * t))
            tri.addLine(to: CGPoint(x: back.x + dy * t, y: back.y - dx * t))
            tri.close()
            tri.fill()
        }
    }
}

/// The pad in SwiftUI.
struct PadOverlay: UIViewRepresentable {
    var geometry: PadGeometry
    var onChange: (DSButtons) -> Void

    func makeUIView(context: Context) -> PadView { PadView(frame: .zero) }

    func updateUIView(_ view: PadView, context: Context) {
        view.geometry = geometry
        view.onChange = onChange
    }

    static func dismantleUIView(_ view: PadView, coordinator: ()) {
        view.releaseAll()
    }
}
#endif
