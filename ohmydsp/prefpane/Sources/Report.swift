import Foundation

/// Plain-text diagnostics report for bug reports. Read-only, needs no privileges, contains no audio,
/// and only log lines that mention OhMyDSP.
enum DMICReport {
    static func build(version: String) -> String {
        let plat = DMICPlatform.detect()
        let status = """
        pane \(version)
        cpu: \(plat?.cpu ?? "?")
        audio controller: \(plat.map { "\($0.name), PCI \($0.pciID), class 0x\(String($0.pciClass, radix: 16)), verdict \($0.verdict)" } ?? "none found")
        firmware: \(DMICFirmware.installed ? "installed" : "not installed")
        kernel extension: \(DMICSystem.kextLoaded() ? "loaded" : "not loaded")
        background service: \(DMICSystem.daemonRunning ? "running" : DMICSystem.daemonInstalled ? "installed, not running" : "not installed")
        audio driver: \(DMICSystem.driverInstalled ? "installed" : "not installed"), device \(DMICAudio.device() != nil ? "visible" : "not visible") to CoreAudio
        """
        let sections: [(String, String)] = [
            ("Status", "echo '\(status)'"),
            ("System", "sw_vers; sysctl -n hw.model machdep.cpu.brand_string; uname -r"),
            ("Boot-args", "nvram boot-args"),
            ("Kernel extension", "kmutil showloaded 2>&1 | grep -i ohmydsp || echo 'not loaded'"),
            ("Installed files", "ls -l '/Library/Application Support/OhMyDSP' '/Library/Audio/Plug-Ins/HAL/OhMyDSP.driver' '/Library/LaunchDaemons/com.ohmydsp.daemon.plist'"),
            ("Log, last 2 hours (newest 1500 lines)", "/usr/bin/log show --last 2h --style compact --predicate 'eventMessage CONTAINS[c] \"ohmydsp\" OR process == \"ohmydspd\"' | tail -n 1500"),
        ]
        return "OhMyDSP report, \(Date())\n\n" + sections.map { "== \($0.0) ==\n\(sh($0.1))\n" }.joined(separator: "\n")
    }

    private static func sh(_ cmd: String) -> String {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/sh"); p.arguments = ["-c", cmd]
        let out = Pipe(); p.standardOutput = out; p.standardError = out
        do { try p.run() } catch { return "(failed: \(error.localizedDescription))" }
        let data = out.fileHandleForReading.readDataToEndOfFile(); p.waitUntilExit()
        return String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
