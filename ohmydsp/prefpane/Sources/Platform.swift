import Foundation
import IOKit

/// The machine's Intel PCH audio controller and what OhMyDSP can do with it. Read-only (IORegistry + sysctl), needs no privileges.
struct DMICPlatform {
    enum Verdict { case supported, notYet, noSOF, dspOff, unknown }
    let device: UInt16, pciClass: UInt16
    let name: String, verdict: Verdict

    var pciID: String { String(format: "8086:%04x", device) }
    var cpu: String {
        var n = 0; sysctlbyname("machdep.cpu.brand_string", nil, &n, nil, 0)
        var b = [CChar](repeating: 0, count: max(n, 1)); sysctlbyname("machdep.cpu.brand_string", &b, &n, nil, 0)
        return String(cString: b)
    }
    /// Why OhMyDSP cannot be installed here, nil when it can.
    var problem: String? {
        switch verdict {
        case .supported: return nil
        case .notYet: return "\(name) (\(pciID)) is not supported by the kernel extension yet."
        case .noSOF: return "\(name) (\(pciID)): Intel's Skylake/Kaby Lake DSP has no Sound Open Firmware, so OhMyDSP cannot drive it."
        case .dspOff: return "The audio DSP is switched off in the firmware setup (HDA mode). Enable the Intel DSP / HD Audio DSP option and restart."
        case .unknown: return "Unknown Intel audio controller (\(pciID)); not supported."
        }
    }

    // PCH audio device IDs (vendor 8086). Only 06c8 is verified on real hardware; the others come from public PCI ID lists and only
    // decide which message the pane shows. A platform becomes .supported only after someone has tested it.
    private static let known: [UInt16: (String, Verdict)] = [
        0x06c8: ("Comet Lake-H", .supported),
        0x02c8: ("Comet Lake-LP", .notYet), 0x9dc8: ("Cannon Point-LP", .notYet), 0xa348: ("Cannon Point-H", .notYet),
        0x34c8: ("Ice Lake-LP", .notYet), 0xa0c8: ("Tiger Lake-LP", .notYet), 0x43c8: ("Tiger Lake-H", .notYet),
        0x4dc8: ("Jasper Lake", .notYet), 0x5a98: ("Apollo Lake", .notYet), 0x3198: ("Gemini Lake", .notYet),
        0x9d70: ("Sunrise Point-LP (Skylake/Kaby Lake)", .noSOF), 0x9d71: ("Sunrise Point-LP (Skylake/Kaby Lake)", .noSOF),
        0xa170: ("Sunrise Point-H (Skylake/Kaby Lake)", .noSOF), 0xa171: ("Sunrise Point-H (Skylake/Kaby Lake)", .noSOF),
        0xa2f0: ("Union Point (Kaby Lake)", .noSOF),
    ]

    /// PCI class 0x0401 = audio controller with the DSP enabled (what the kext matches); 0x0403 = plain HD Audio, DSP hidden.
    static func detect() -> DMICPlatform? {
        var it: io_iterator_t = 0
        guard IOServiceGetMatchingServices(0, IOServiceMatching("IOPCIDevice"), &it) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(it) }
        while true {
            let s = IOIteratorNext(it); if s == 0 { return nil }
            defer { IOObjectRelease(s) }
            guard word(s, "vendor-id") == 0x8086, let dev = word(s, "device-id"), let code = word(s, "class-code") else { continue }
            let cls = UInt16((code >> 8) & 0xffff)
            guard cls == 0x0401 || cls == 0x0403 else { continue }
            let id = UInt16(truncatingIfNeeded: dev)
            let k = known[id]
            let verdict: Verdict = k == nil ? .unknown : (cls == 0x0403 && k!.1 != .noSOF) ? .dspOff : k!.1
            return DMICPlatform(device: id, pciClass: cls, name: k?.0 ?? "Intel audio controller", verdict: verdict)
        }
    }

    private static func word(_ s: io_registry_entry_t, _ key: String) -> UInt32? {
        guard let d = IORegistryEntryCreateCFProperty(s, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? Data, d.count >= 4 else { return nil }
        return d.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
    }
}
