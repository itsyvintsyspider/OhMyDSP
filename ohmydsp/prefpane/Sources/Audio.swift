// CoreAudio access for the menu bar app: finds the OhMyDSP input device and reads/sets its volume and mute through the standard
// device properties (the driver's volume/mute controls are mapped onto them by the HAL), so System Settings and this app stay in sync.
import CoreAudio
import Foundation
import IOKit

enum DMICAudio {
    static let deviceUID = "com.ohmydsp.InternalMicrophone"
    static let dbMin: Float = -28, dbMax: Float = 22

    private static func addr(_ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ el: UInt32 = kAudioObjectPropertyElementMain) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: el)
    }

    static func device() -> AudioObjectID? {
        var a = addr(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size) == 0 else { return nil }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &ids) == 0 else { return nil }
        for id in ids {
            var ua = addr(kAudioDevicePropertyDeviceUID)
            var uid: Unmanaged<CFString>?
            var s = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            if AudioObjectGetPropertyData(id, &ua, 0, nil, &s, &uid) == 0, let u = uid?.takeRetainedValue() as String?, u == deviceUID { return id }
        }
        return nil
    }

    static func gainDb(_ dev: AudioObjectID) -> Float? {
        var a = addr(kAudioDevicePropertyVolumeDecibels, kAudioObjectPropertyScopeInput)
        var v: Float32 = 0; var s = UInt32(4)
        return AudioObjectGetPropertyData(dev, &a, 0, nil, &s, &v) == 0 ? v : nil
    }

    @discardableResult static func setGainDb(_ dev: AudioObjectID, _ db: Float) -> Bool {
        var a = addr(kAudioDevicePropertyVolumeDecibels, kAudioObjectPropertyScopeInput)
        var v = min(max(db, dbMin), dbMax)
        return AudioObjectSetPropertyData(dev, &a, 0, nil, 4, &v) == 0
    }

    static func muted(_ dev: AudioObjectID) -> Bool? {
        var a = addr(kAudioDevicePropertyMute, kAudioObjectPropertyScopeInput)
        var v: UInt32 = 0; var s = UInt32(4)
        return AudioObjectGetPropertyData(dev, &a, 0, nil, &s, &v) == 0 ? v != 0 : nil
    }

    @discardableResult static func setMuted(_ dev: AudioObjectID, _ m: Bool) -> Bool {
        var a = addr(kAudioDevicePropertyMute, kAudioObjectPropertyScopeInput)
        var v: UInt32 = m ? 1 : 0
        return AudioObjectSetPropertyData(dev, &a, 0, nil, 4, &v) == 0
    }

    static func running(_ dev: AudioObjectID) -> Bool {
        var a = addr(kAudioDevicePropertyDeviceIsRunningSomewhere)
        var v: UInt32 = 0; var s = UInt32(4)
        return AudioObjectGetPropertyData(dev, &a, 0, nil, &s, &v) == 0 && v != 0
    }

    /// Calls `handler` (on the main queue) whenever the device list, the gain or the mute state changes.
    static func observe(_ handler: @escaping () -> Void) {
        var list = addr(kAudioHardwarePropertyDevices)
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &list, .main) { _, _ in handler() }
        guard let dev = device() else { return }
        for sel in [kAudioDevicePropertyVolumeDecibels, kAudioDevicePropertyMute, kAudioDevicePropertyDeviceIsRunningSomewhere] {
            var a = addr(sel, sel == kAudioDevicePropertyDeviceIsRunningSomewhere ? kAudioObjectPropertyScopeGlobal : kAudioObjectPropertyScopeInput)
            AudioObjectAddPropertyListenerBlock(dev, &a, .main) { _, _ in handler() }
        }
    }
}

enum DMICSystem {
    /// True when the kext's IOService is in the registry (readable without privileges).
    static func kextLoaded() -> Bool {
        let svc = IOServiceGetMatchingService(0, IOServiceMatching("OhMyDSP"))
        defer { if svc != 0 { IOObjectRelease(svc) } }
        return svc != 0
    }
    static var daemonRunning: Bool {
        let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep"); p.arguments = ["-x", "ohmydspd"]
        p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        do { try p.run(); p.waitUntilExit(); return p.terminationStatus == 0 } catch { return false }
    }
    static var daemonInstalled: Bool { FileManager.default.fileExists(atPath: "/Library/LaunchDaemons/com.ohmydsp.daemon.plist") }
    static var driverInstalled: Bool { FileManager.default.fileExists(atPath: "/Library/Audio/Plug-Ins/HAL/OhMyDSP.driver") }
}
