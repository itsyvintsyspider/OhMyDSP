import Foundation
import CryptoKit

/// The Sound Open Firmware image for Comet Lake / Cannon Lake: where it lives, and how it is fetched.
enum DMICFirmware {
    static let dir = "/Library/Application Support/OhMyDSP"
    static let file = "sof-cnl.ri"
    static let size = 557_824
    static let sha256 = "c5119cfca9c4bdf5ab87a87af08fb8fb2bf92767e0f070ea4eb52a877f10359e"   // intel-signed/sof-cnl.ri, identical in sof-bin 2025.05 .. 2026.09.2

    static var installed: Bool {
        (try? FileManager.default.attributesOfItem(atPath: "\(dir)/\(file)")[.size] as? Int) == size
    }
}

struct FirmwareError: LocalizedError { let errorDescription: String? }

/// Downloads the pinned sof-bin release over HTTPS, checks the tarball and the extracted image against known SHA-256 values, and leaves
/// `sof-cnl.ri` + `LICENCE.Intel` in a temporary folder. Nothing is installed here; the installer copies the folder (and re-checks the hash).
/// Callbacks arrive on the main queue.
final class FirmwareDownload: NSObject, URLSessionDownloadDelegate {
    private static let release = "2026.09.2"
    private static let url = URL(string: "https://github.com/thesofproject/sof-bin/releases/download/v\(release)/sof-bin-\(release).tar.gz")!
    private static let tarballSize: Int64 = 17_550_180
    private static let tarballSHA256 = "5f7bc30b2b5c1e6b0da55a33fc44e2b7f56ae9b06952edcde614f51ef6b943e1"

    private let onProgress: (Double) -> Void, onDone: (Result<URL, Error>) -> Void
    private var session: URLSession?
    private var finished = false

    init(progress: @escaping (Double) -> Void, done: @escaping (Result<URL, Error>) -> Void) { onProgress = progress; onDone = done }

    func start() {
        session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: .main)
        session?.downloadTask(with: Self.url).resume()
    }
    func cancel() { finished = true; session?.invalidateAndCancel() }

    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64, totalBytesWritten w: Int64, totalBytesExpectedToWrite e: Int64) {
        onProgress(min(1, Double(w) / Double(e > 0 ? e : Self.tarballSize)))
    }
    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard !finished else { return }
        finished = true
        do { onDone(.success(try Self.unpack(location))) } catch { onDone(.failure(error)) }      // the temp file is removed when this returns
    }
    func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard !finished, let error = error else { return }
        finished = true; onDone(.failure(error))
    }

    private static func hex(_ d: some Sequence<UInt8>) -> String { d.map { String(format: "%02x", $0) }.joined() }

    private static func unpack(_ tarball: URL) throws -> URL {
        let data = try Data(contentsOf: tarball, options: .mappedIfSafe)
        guard hex(SHA256.hash(data: data)) == tarballSHA256 else { throw FirmwareError(errorDescription: "The download does not match the expected checksum, so it was discarded.") }
        let out = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("ohmydsp-fw-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
        let root = "sof-bin-\(release)"
        let tar = Process(); tar.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        tar.arguments = ["-xzf", tarball.path, "-C", out.path, "--strip-components=1", "\(root)/sof/intel-signed/\(DMICFirmware.file)", "\(root)/LICENCE.Intel"]
        try tar.run(); tar.waitUntilExit()
        let ri = out.appendingPathComponent("sof/intel-signed/\(DMICFirmware.file)")
        guard tar.terminationStatus == 0, let fw = try? Data(contentsOf: ri), fw.count == DMICFirmware.size, hex(SHA256.hash(data: fw)) == DMICFirmware.sha256
        else { throw FirmwareError(errorDescription: "The firmware image in the download is not the expected file.") }
        try FileManager.default.moveItem(at: ri, to: out.appendingPathComponent(DMICFirmware.file))
        return out
    }
}
