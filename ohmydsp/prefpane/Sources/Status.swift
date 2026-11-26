import Foundation
import IOKit
import CryptoKit

enum Health { case ok, bad, idle }
struct PaneStatus {
    let health: Health
    let text: String
}

/// Read-only observations. Rendering is separate so missing/fault states can be tested offline.
struct DMICStatus {
    static let sessionID = "com.ohmydsp.OhMyDSP.IPC4Session"
    var bundle: String?
    var ready: Bool?
    var state: String?
    var session: UInt64?
    var initialIdle = false
    var daemon = false
    var visible = false
    var audioRunning = false
    var assets = PaneStatus(health: .idle, text: "Not checked")
    var startup = PaneStatus(health: .idle, text: "Not checked")
    var startupLog: String?
    var isSession: Bool { bundle == Self.sessionID }

    var kernelStatus: PaneStatus {
        guard let bundle = bundle else { return PaneStatus(health: .bad, text: "Not loaded") }
        return PaneStatus(health: isSession ? .ok : .idle, text: isSession ? "IPC4 Session loaded" : "Other build loaded: \(bundle)")
    }
    var sessionStatus: PaneStatus {
        guard isSession else { return PaneStatus(health: .idle, text: "Unavailable") }
        if ready == false { return PaneStatus(health: .bad, text: "Initialization failed") }
        guard ready == true else { return PaneStatus(health: .idle, text: "Initializing…") }
        let suffix = session.map { " · #\($0)" } ?? ""
        switch state {
        case "running": return PaneStatus(health: .ok, text: "Recording" + suffix)
        case "stopped": return PaneStatus(health: .ok, text: "Idle" + suffix)
        case "starting": return PaneStatus(health: .idle, text: "Starting" + suffix)
        case "stopping": return PaneStatus(health: .idle, text: "Stopping" + suffix)
        case nil where initialIdle: return PaneStatus(health: .ok, text: "Prepared · no recording yet")
        case nil: return PaneStatus(health: .idle, text: "State unavailable")
        default: return PaneStatus(health: .bad, text: (state ?? "Unknown") + suffix)
        }
    }
    var microphoneStatus: PaneStatus {
        guard isSession, ready == true else { return PaneStatus(health: .bad, text: "Capture unavailable") }
        guard daemon else { return PaneStatus(health: .bad, text: "Service unavailable") }
        guard visible else { return PaneStatus(health: .bad, text: "Audio device unavailable") }
        guard sessionStatus.health == .ok else { return sessionStatus }
        if state == "running" { return PaneStatus(health: .ok, text: "In use") }
        if audioRunning { return PaneStatus(health: .idle, text: "Waiting for capture") }
        return PaneStatus(health: .ok, text: "Ready · idle")
    }
    var report: String {
        "kernel bundle: \(bundle ?? "absent")\nIPC4 ready: \(ready.map(String.init) ?? "unknown")\nIPC4 session: \(sessionStatus.text)\nRPL assets: \(assets.text)\nstartup: \(startup.text)\nstartup log: \(startupLog ?? "none")\nmicrophone: \(microphoneStatus.text)"
    }

    static func read() -> DMICStatus {
        var result = DMICStatus()
        let svc = IOServiceGetMatchingService(0, IOServiceMatching("OhMyDSP"))
        if svc != 0 {
            defer { IOObjectRelease(svc) }
            func property(_ key: String) -> Any? {
                IORegistryEntryCreateCFProperty(svc, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
            }
            result.bundle = property("CFBundleIdentifier") as? String
            result.ready = property("ipc4-session-ready") as? Bool
            result.state = property("ipc4-session-state") as? String
            result.session = (property("ipc4-session-number") as? NSNumber)?.uint64Value
            if result.isSession && result.ready == true && result.state == nil && result.session == nil {
                let log = property("phase1-log") as? [String] ?? []
                result.initialIdle = property("phase1-log-truncated") as? Bool != true &&
                    log.contains("live capture ready: DSP idle, powers up when the user client starts the stream") &&
                    !log.contains(where: { $0.hasPrefix("IPC4 session ") })
            }
        }
        result.daemon = DMICSystem.daemonRunning
        if let dev = DMICAudio.device() { result.visible = true; result.audioRunning = DMICAudio.running(dev) }
        result.assets = rplAssets()
        let receipt = startupReceipt()
        result.startup = receipt.0; result.startupLog = receipt.1
        return result
    }

    static func rplAssets(directory: String = "/Library/Application Support/OhMyDSP/rpl") -> PaneStatus {
        let expected = ["sof-rpl.ri": "351325786e53ca7cd23ffc1462d82913f3e0c08ad78f89053d85e4b6703cf12b",
                        "rpl-dmic.i4pc": "4cb92661b2969e2d9bf394b31945a75a54827204db60575b4ca8dde4f72e2236"]
        for name in expected.keys.sorted() {
            guard let data = try? Data(contentsOf: URL(fileURLWithPath: directory).appendingPathComponent(name)) else {
                return PaneStatus(health: .bad, text: "Missing/unreadable: \(name)")
            }
            guard SHA256.hash(data: data).map({ String(format: "%02x", $0) }).joined() == expected[name] else {
                return PaneStatus(health: .bad, text: "Unverified: \(name)")
            }
        }
        return PaneStatus(health: .ok, text: "RPL firmware + graph verified")
    }

    static func startupStatus(installed: Bool, currentBoot: String?, log: String?, exit: String?) -> PaneStatus {
        guard installed else { return PaneStatus(health: .idle, text: "Not installed") }
        guard let boot = currentBoot, let log = log,
              log.components(separatedBy: .newlines).contains("boot_uuid=\(boot)") else {
            return PaneStatus(health: .idle, text: "No receipt for this boot")
        }
        guard let exit = exit else { return PaneStatus(health: .idle, text: "Pending · inspect startup log") }
        guard exit.trimmingCharacters(in: .whitespacesAndNewlines) == "0",
              log.contains("PASS: session ready; recording remains client-controlled") else {
            return PaneStatus(health: .bad, text: "Startup failed · inspect log")
        }
        let lines = log.components(separatedBy: .newlines)
        let markers = lines.filter { $0.hasPrefix("load_action=") }.map { String($0.dropFirst("load_action=".count)) }
        var legacy = [String]()
        if lines.contains("loading qualified session kext") { legacy.append("loaded") }
        if lines.contains("expected session already loaded; no reload") { legacy.append("already_loaded") }
        let actions = markers.isEmpty ? legacy : markers
        if legacy.isEmpty || actions == legacy {
            if actions == ["loaded"] { return PaneStatus(health: .ok, text: "Loaded this boot") }
            if actions == ["already_loaded"] { return PaneStatus(health: .ok, text: "Ready this boot · load skipped") }
        }
        return PaneStatus(health: .idle, text: "Load action unverified")
    }
    private static func startupReceipt() -> (PaneStatus, String?) {
        let fm = FileManager.default
        let installed = fm.fileExists(atPath: "/Library/LaunchDaemons/com.ohmydsp.startup.plist")
        var bytes = [CChar](repeating: 0, count: 64); var size = bytes.count
        guard sysctlbyname("kern.bootsessionuuid", &bytes, &size, nil, 0) == 0 else {
            return (startupStatus(installed: installed, currentBoot: nil, log: nil, exit: nil), nil)
        }
        let boot = String(cString: bytes)
        let root = URL(fileURLWithPath: "/Library/Logs/OhMyDSP/startup")
        let dirs = (try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.creationDateKey])) ?? []
        let recent = dirs.filter { $0.lastPathComponent.hasPrefix(boot + ".") }.max {
            ((try? $0.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast) <
            ((try? $1.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast)
        }
        guard let dir = recent else { return (startupStatus(installed: installed, currentBoot: boot, log: nil, exit: nil), nil) }
        let logURL = dir.appendingPathComponent("loader.log")
        // Bound reads; this pane never reads PCM or opens a capture client.
        func read(_ url: URL) -> String? {
            guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
            defer { try? handle.close() }
            guard let data = try? handle.read(upToCount: 131072) else { return nil }
            return String(data: data, encoding: .utf8)
        }
        return (startupStatus(installed: installed, currentBoot: boot, log: read(logURL), exit: read(dir.appendingPathComponent("exit-status"))), logURL.path)
    }
}
