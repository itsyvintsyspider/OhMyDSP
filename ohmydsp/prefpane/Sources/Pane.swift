// OhMyDSP preference pane: status of the pieces (kernel extension, background service, audio driver, microphone), input volume / mute
// (through CoreAudio, same values as System Settings > Sound), and install / uninstall of the userspace part with the standard admin prompt.
import PreferencePanes
import UniformTypeIdentifiers
import AppKit

private enum Health { case ok, bad, idle }

private final class StatusRow: NSStackView {
    let icon = NSImageView(), title: NSTextField, detail = NSTextField(labelWithString: "")
    init(_ name: String) {
        title = NSTextField(labelWithString: name)
        super.init(frame: .zero)
        orientation = .horizontal; alignment = .centerY; spacing = 8
        icon.imageScaling = .scaleProportionallyDown
        icon.widthAnchor.constraint(equalToConstant: 18).isActive = true
        detail.textColor = .secondaryLabelColor; detail.alignment = .right
        let spacer = NSView(); spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        for v in [icon, title, spacer, detail] { addArrangedSubview(v) }
    }
    required init?(coder: NSCoder) { fatalError() }
    func set(_ h: Health, _ text: String) {
        let (name, color): (String, NSColor) = h == .ok ? ("checkmark.circle.fill", .systemGreen) : h == .bad ? ("xmark.circle.fill", .systemRed) : ("minus.circle", .secondaryLabelColor)
        icon.image = NSImage(systemSymbolName: name, accessibilityDescription: text)?.withSymbolConfiguration(.init(paletteColors: h == .idle ? [color] : [.white, color]))
        detail.stringValue = text
        setAccessibilityLabel("\(title.stringValue): \(text)")
    }
}

// Rounded grouped container in the style of System Settings sections; colours are semantic and re-resolved on appearance changes.
private final class CardView: NSView {
    override init(frame: NSRect) { super.init(frame: frame); wantsLayer = true; translatesAutoresizingMaskIntoConstraints = false }
    required init?(coder: NSCoder) { fatalError() }
    override var wantsUpdateLayer: Bool { true }
    override func updateLayer() {
        layer?.cornerRadius = 10; layer?.borderWidth = 1
        effectiveAppearance.performAsCurrentDrawingAppearance {
            layer?.backgroundColor = NSColor.controlBackgroundColor.cgColor
            layer?.borderColor = NSColor.separatorColor.cgColor
        }
    }
}

@objc(OhMyDSPPane)
final class OhMyDSPPane: NSPreferencePane {
    private let kext = StatusRow("Kernel Extension"), service = StatusRow("Background Service"), driver = StatusRow("Audio Driver"), mic = StatusRow("Microphone")
    private let hardware = StatusRow("Hardware"), firmware = StatusRow("Firmware")
    private let progress = NSProgressIndicator(), progressLabel = NSTextField(labelWithString: "")
    private var download: FirmwareDownload?
    private var restartHint = false
    private var progressBox: NSView?
    private let slider = NSSlider(value: 9, minValue: Double(DMICAudio.dbMin), maxValue: Double(DMICAudio.dbMax), target: nil, action: nil)
    private let valueLabel = NSTextField(labelWithString: "")
    private let muteBox = NSButton(checkboxWithTitle: "Mute", target: nil, action: nil)
    private let installButton = NSButton(title: "Install…", target: nil, action: nil)
    private let uninstallButton = NSButton(title: "Uninstall…", target: nil, action: nil)
    private let hint = NSTextField(wrappingLabelWithString: "")
    private let logButton = NSButton(title: "Collect Logs…", target: nil, action: nil)
    private var collecting = false
    private var timer: Timer?
    private var busy = false

    override func loadMainView() -> NSView {
        let content = NSStackView(views: [
            section("Status", [kext, hardware, firmware, service, driver, mic]),
            section("Input", [volumeRow(), muteBox]),
            section("Setup", [buttonRow(), progressRow(), hint]),
            section("Misc", [logRow()])
        ])
        content.orientation = .vertical; content.alignment = .leading; content.spacing = 20
        content.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        content.translatesAutoresizingMaskIntoConstraints = false
        let root = NSView(frame: NSRect(x: 0, y: 0, width: 668, height: 650))
        root.addSubview(content)
        NSLayoutConstraint.activate([content.topAnchor.constraint(equalTo: root.topAnchor), content.leadingAnchor.constraint(equalTo: root.leadingAnchor), content.trailingAnchor.constraint(equalTo: root.trailingAnchor)])
        for sec in content.arrangedSubviews { sec.widthAnchor.constraint(equalTo: content.widthAnchor, constant: -40).isActive = true }
        slider.target = self; slider.action = #selector(sliderMoved); slider.isContinuous = true; slider.setAccessibilityLabel("Input volume")
        muteBox.target = self; muteBox.action = #selector(muteToggled)
        installButton.target = self; installButton.action = #selector(install); uninstallButton.target = self; uninstallButton.action = #selector(uninstall)
        logButton.target = self; logButton.action = #selector(collectLogs)
        progress.minValue = 0; progress.maxValue = 1; progress.isIndeterminate = false; progress.controlSize = .small
        progressLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize); progressLabel.textColor = .secondaryLabelColor
        hint.font = .systemFont(ofSize: NSFont.smallSystemFontSize); hint.textColor = .secondaryLabelColor
        valueLabel.textColor = .secondaryLabelColor; valueLabel.alignment = .right
        valueLabel.widthAnchor.constraint(equalToConstant: 64).isActive = true
        mainView = root
        return root
    }

    override func mainViewDidLoad() { refresh() }
    override func willSelect() {
        refresh(); DMICAudio.observe { [weak self] in self?.refresh() }
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.refresh() }
    }
    override func didUnselect() { timer?.invalidate(); timer = nil }

    // MARK: layout helpers
    private func section(_ name: String, _ rows: [NSView]) -> NSView {
        let header = NSTextField(labelWithString: name); header.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        let stack = NSStackView(views: rows); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        let card = CardView(); card.addSubview(stack)
        NSLayoutConstraint.activate([stack.topAnchor.constraint(equalTo: card.topAnchor, constant: 12), stack.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -12),
                                     stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 14), stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -14)])
        for r in rows { r.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true }
        let outer = NSStackView(views: [header, card]); outer.orientation = .vertical; outer.alignment = .leading; outer.spacing = 6
        card.widthAnchor.constraint(equalTo: outer.widthAnchor).isActive = true
        return outer
    }
    private func volumeRow() -> NSView {
        let title = NSTextField(labelWithString: "Input Volume")
        let r = NSStackView(views: [title, slider, valueLabel]); r.orientation = .horizontal; r.alignment = .centerY; r.spacing = 10
        slider.setContentHuggingPriority(.defaultLow, for: .horizontal)
        return r
    }
    private func buttonRow() -> NSView {
        let sound = NSButton(title: "Sound Settings…", target: self, action: #selector(openSound))
        let spacer = NSView(); spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let r = NSStackView(views: [installButton, uninstallButton, spacer, sound]); r.orientation = .horizontal; r.spacing = 8
        return r
    }

    private func progressRow() -> NSView {
        let r = NSStackView(views: [progress, progressLabel]); r.orientation = .horizontal; r.alignment = .centerY; r.spacing = 10
        progress.setContentHuggingPriority(.defaultLow, for: .horizontal); progressLabel.widthAnchor.constraint(equalToConstant: 130).isActive = true
        r.isHidden = true; progressBox = r
        return r
    }
    private func logRow() -> NSView {
        let note = NSTextField(labelWithString: "Saves a text report (no audio) to attach to a bug report.")
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize); note.textColor = .secondaryLabelColor
        let r = NSStackView(views: [logButton, note]); r.orientation = .horizontal; r.alignment = .centerY; r.spacing = 10
        return r
    }

    // MARK: state
    private func refresh() {
        let loaded = DMICSystem.kextLoaded(), dev = DMICAudio.device()
        kext.set(loaded ? .ok : .bad, loaded ? "Loaded" : "Not loaded")
        service.set(DMICSystem.daemonRunning ? .ok : DMICSystem.daemonInstalled ? .bad : .idle, DMICSystem.daemonRunning ? "Running" : DMICSystem.daemonInstalled ? "Not running" : "Not installed")
        driver.set(dev != nil ? .ok : DMICSystem.driverInstalled ? .idle : .idle, dev != nil ? "Active" : DMICSystem.driverInstalled ? "Installed" : "Not installed")
        if let d = dev {
            mic.set(.ok, DMICAudio.running(d) ? "In use" : "Ready")
            let db = DMICAudio.gainDb(d) ?? 0
            valueLabel.stringValue = String(format: "%+.1f dB", db); slider.doubleValue = Double(db); slider.isEnabled = true
            muteBox.isEnabled = true; muteBox.state = DMICAudio.muted(d) == true ? .on : .off
        } else {
            mic.set(.idle, "Not available"); valueLabel.stringValue = "—"; slider.isEnabled = false; muteBox.isEnabled = false; muteBox.state = .off
        }
        let installed = DMICSystem.driverInstalled || DMICSystem.daemonInstalled
        let plat = DMICPlatform.detect(), fw = DMICFirmware.installed, downloading = download != nil
        if let p = plat {
            hardware.set(p.verdict == .supported ? .ok : p.verdict == .notYet || p.verdict == .unknown ? .idle : .bad,
                         p.verdict == .supported ? p.name : p.verdict == .dspOff ? "DSP off in firmware setup" : p.verdict == .noSOF ? "\(p.name): no SOF firmware" : "\(p.name): not supported yet")
            hardware.toolTip = "\(p.cpu)\nPCI \(p.pciID), class 0x\(String(p.pciClass, radix: 16))"
        } else { hardware.set(.bad, "No Intel audio controller"); hardware.toolTip = nil }
        firmware.set(fw ? .ok : .idle, fw ? "Installed" : downloading ? "Downloading…" : "Not installed")
        installButton.title = installed ? "Reinstall…" : "Install…"
        installButton.isEnabled = !busy && !downloading && plat?.verdict == .supported; uninstallButton.isEnabled = installed && !busy && !downloading
        hint.stringValue = plat == nil ? "No Intel audio controller was found." : plat?.problem ??
            (!loaded ? "The kernel extension is loaded at boot by OpenCore: add OhMyDSP.kext to Kernel > Add, after Lilu, and restart." :
             !fw || !installed ? "Install… downloads the SOF firmware if needed (17 MB, checked), then installs the background service and the audio driver." :
             dev == nil && restartHint ? "Restart your Mac once so the kernel extension can load the firmware." : "")
        hint.isHidden = hint.stringValue.isEmpty
    }

    @objc private func sliderMoved() {
        guard let d = DMICAudio.device() else { return }
        let db = Float((slider.doubleValue * 2).rounded() / 2)       // 0.5 dB steps
        DMICAudio.setGainDb(d, db); valueLabel.stringValue = String(format: "%+.1f dB", db)
    }
    @objc private func muteToggled() { if let d = DMICAudio.device() { DMICAudio.setMuted(d, muteBox.state == .on) } }
    @objc private func openSound() { NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.Sound-Settings.extension")!) }

    @objc private func collectLogs() {
        guard !collecting else { return }
        let panel = NSSavePanel()
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        panel.nameFieldStringValue = "OhMyDSP-report-\(f.string(from: Date())).txt"; panel.allowedContentTypes = [.plainText]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        collecting = true; logButton.title = "Collecting…"; logButton.isEnabled = false
        let version = (Bundle(for: OhMyDSPPane.self).infoDictionary?["CFBundleShortVersionString"] as? String) ?? "?"
        DispatchQueue.global().async {
            let ok = (try? DMICReport.build(version: version).write(to: url, atomically: true, encoding: .utf8)) != nil
            DispatchQueue.main.async {
                self.collecting = false; self.logButton.title = "Collect Logs…"; self.logButton.isEnabled = true
                if ok { NSWorkspace.shared.activateFileViewerSelecting([url]) }
                else { let a = NSAlert(); a.messageText = "Could not save the report"; a.alertStyle = .warning; a.runModal() }
            }
        }
    }

    // MARK: install / uninstall (standard administrator prompt)
    @objc private func install() {
        guard download == nil else { return }
        if DMICFirmware.installed { runSetup(arg: "", firmware: nil); return }
        progress.doubleValue = 0; progressLabel.stringValue = "Downloading firmware…"; progressBox?.isHidden = false
        download = FirmwareDownload(progress: { [weak self] f in self?.progress.doubleValue = f; self?.progressLabel.stringValue = "Downloading… \(Int(f * 100))%" },
                                    done: { [weak self] r in
            guard let self = self else { return }
            self.download = nil; self.progressBox?.isHidden = true
            switch r {
            case .success(let dir): self.runSetup(arg: "", firmware: dir)
            case .failure(let e):
                let a = NSAlert(); a.messageText = "Could not download the firmware"; a.informativeText = e.localizedDescription + "\nCheck your internet connection and try again."
                a.alertStyle = .warning; a.runModal(); self.refresh()
            }
        })
        download?.start(); refresh()
    }
    @objc private func uninstall() { runSetup(arg: "uninstall", firmware: nil) }
    private func runSetup(arg: String, firmware: URL?) {
        guard !busy, let res = Bundle(for: OhMyDSPPane.self).resourcePath else { return }
        busy = true; refresh()
        let fw = firmware.map { "FIRMWARE_DIR='\($0.path)' FIRMWARE_SHA256='\(DMICFirmware.sha256)' " } ?? ""
        let cmd = "\(fw)PAYLOAD='\(res)' /bin/sh '\(res)/install-userspace.sh' \(arg)"
        DispatchQueue.global().async {
            var err: NSDictionary?
            let script = NSAppleScript(source: "do shell script \"\(cmd)\" with administrator privileges")
            _ = script?.executeAndReturnError(&err)
            DispatchQueue.main.async {
                self.busy = false
                if let d = firmware { try? FileManager.default.removeItem(at: d); if err == nil { self.restartHint = true } }
                if let err = err, (err[NSAppleScript.errorNumber] as? Int) != -128 {          // -128 = the user cancelled the password prompt
                    let a = NSAlert(); a.messageText = "\(arg.isEmpty ? "Installation" : "Removal") failed"
                    a.informativeText = (err[NSAppleScript.errorMessage] as? String) ?? "Unknown error"; a.alertStyle = .warning; a.runModal()
                }
                self.refresh()
            }
        }
    }
}
