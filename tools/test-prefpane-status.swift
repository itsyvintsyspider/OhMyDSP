import Foundation

@main struct StatusTests {
    static func main() throws {
        assert(DMICPlatform.profile(device: 0x51ca, subsystemVendor: 0x17aa, subsystemID: 0x50d1) != nil)
        assert(DMICPlatform.profile(device: 0x51ca, subsystemVendor: 0x17aa, subsystemID: 0x50d2) == nil)
        assert(DMICPlatform.profile(device: 0x51ca, subsystemVendor: nil, subsystemID: nil) == nil)
        assert(DMICPlatform.profile(device: 0x06c8, subsystemVendor: 0x17aa, subsystemID: 0x50d1) == nil)
        func actions(_ device: UInt16?, _ verdict: DMICPlatform.Verdict,
                     session: Bool = false, markers: Bool = false, busy: Bool = false,
                     installed: Bool = true) -> (Bool, Bool) {
            let platform = device.map { DMICPlatform(device: $0, pciClass: 0, name: "fixture", verdict: verdict, profile: nil) }
            return DMICPlatform.setupActions(platform: platform, session: session, rplFiles: markers, installed: installed, busy: busy)
        }
        assert(actions(0x06c8, .supported) == (true, true))
        assert(actions(0x06c8, .dspOff) == (false, true))
        assert(actions(nil, .unknown) == (false, true))
        assert(actions(nil, .unknown, markers: true) == (false, false))
        assert(actions(nil, .unknown, session: true) == (false, false))
        assert(actions(0x51ca, .dspOff) == (false, false))
        assert(actions(0x06c8, .supported, busy: true) == (false, false))
        assert(actions(0x06c8, .supported, installed: false) == (true, false))
        var s = DMICStatus()
        s.visible = true; s.daemon = true
        assert(s.microphoneStatus.health == .bad) // visible HAL without kernel must never say Ready
        s.bundle = "com.ohmydsp.OhMyDSP.IPC4Probe"; s.ready = true
        assert(s.microphoneStatus.health == .bad)
        s.bundle = DMICStatus.sessionID; s.ready = nil
        assert(s.sessionStatus.health == .idle && s.microphoneStatus.health == .bad)
        s.ready = false; assert(s.sessionStatus.health == .bad)
        s.ready = true
        assert(s.sessionStatus.health == .idle && s.microphoneStatus.health != .ok)
        s.initialIdle = true
        assert(s.sessionStatus.health == .ok && s.microphoneStatus.text == "Ready · idle")
        s.state = "stopped"; s.session = 3
        assert(s.sessionStatus.text == "Idle · #3" && s.microphoneStatus.health == .ok)
        s.daemon = false; assert(s.microphoneStatus.health == .bad); s.daemon = true
        s.visible = false; assert(s.microphoneStatus.health == .bad); s.visible = true
        s.state = "running"; assert(s.microphoneStatus.text == "In use")
        for state in ["start-failed", "stop-failed", "fault", "unexpected"] {
            s.state = state; assert(s.sessionStatus.health == .bad && s.microphoneStatus.health == .bad)
        }
        s.state = "starting"; assert(s.microphoneStatus.health == .idle)
        s.state = "stopped"; s.audioRunning = true; assert(s.microphoneStatus.health == .idle)
        let log = "boot_uuid=NEW\nloading qualified session kext\nPASS: session ready; recording remains client-controlled\n"
        func receipt(_ boot: String?, _ text: String?, _ exit: String?, installed: Bool = true) -> PaneStatus {
            DMICStatus.startupStatus(installed: installed, currentBoot: boot, log: text, exit: exit)
        }
        assert(receipt("NEW", log, "0\n").health == .ok)
        let skipped = log.replacingOccurrences(of: "loading qualified session kext", with: "load_action=already_loaded")
        assert(receipt("NEW", skipped, "0").text.contains("skipped"))
        assert(receipt("NEW", log + "load_action=already_loaded\n", "0").health == .idle)
        assert(receipt("OLD", log, "0").health == .idle)
        assert(receipt(nil, log, "0").health == .idle)
        assert(receipt("NEW", nil, nil).health == .idle)
        assert(receipt("NEW", log, nil).health == .idle)
        assert(receipt("NEW", log, "1").health == .bad)
        assert(receipt("NEW", "boot_uuid=NEW\n", "0").health == .bad)
        assert(receipt("NEW", log, "0", installed: false).health == .idle)
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        assert(DMICStatus.rplAssets(directory: dir.path).health == .bad)
        try Data("wrong".utf8).write(to: dir.appendingPathComponent("rpl-dmic.i4pc"))
        assert(DMICStatus.rplAssets(directory: dir.path).text.hasPrefix("Unverified"))
        print("PASS: RPL initial/idle/active/fault states, visible HAL without capture, startup receipt correlation and asset rejection")
        if CommandLine.arguments.contains("--live") {
            print("Live read-only snapshot:\n\(DMICStatus.read().report)")
        }
    }
}
