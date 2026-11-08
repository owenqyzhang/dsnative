import DSNCore
import Darwin

/// Frame deadlines at a fixed rate. Each deadline is one period after the last, so rounding in
/// the sleeps doesn't accumulate; after a stall it starts afresh rather than running a burst
/// of catch-up frames.
public struct FramePacer: Sendable {
    /// The DS's refresh rate: 263 lines of 2130 bus cycles at 33.513982 MHz, about 59.8261 Hz.
    public static let dsFrameRate = Double(DSN_BUS_HZ) / Double(DSN_FRAME_CYCLES)

    public let period: Double
    /// How far behind a deadline may fall before the pacer gives up on it.
    public let maxLag: Double
    private var deadline: Double?

    public init(rate: Double = FramePacer.dsFrameRate, maxLagFrames: Double = 3) {
        period = 1 / rate
        maxLag = period * maxLagFrames
    }

    /// When the next frame should start, given that the last one finished at `now` (seconds).
    public mutating func nextDeadline(now: Double) -> Double {
        var next = (deadline ?? now) + period
        if now - next > maxLag { next = now }
        deadline = next
        return next
    }

    /// Forget the schedule (after a pause or a single step).
    public mutating func reset() { deadline = nil }
}

/// The host's monotonic clock, and sleeping until a time on it.
enum HostClock {
    private static let timebase: mach_timebase_info_data_t = {
        var t = mach_timebase_info_data_t()
        mach_timebase_info(&t)
        return t
    }()

    static func now() -> Double {
        Double(mach_absolute_time()) * Double(timebase.numer) / Double(timebase.denom) / 1e9
    }

    static func sleep(until t: Double) {
        let ticks = t * 1e9 * Double(timebase.denom) / Double(timebase.numer)
        if ticks > Double(mach_absolute_time()) { mach_wait_until(UInt64(ticks)) }
    }
}
