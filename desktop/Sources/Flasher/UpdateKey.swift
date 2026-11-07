// The key that signs firmware for the Bluetooth update (docs/desktop-app-design.md 13.11; K1, you chose 2026-11-07: a new
// key made by the App, only on this Mac). It is made in this Mac's Secure Enclave: the private key never leaves it. What is
// stored is the Enclave's own encrypted handle, useless on any other Mac, in a file of the user (0600). Not the keychain: an
// ad-hoc signed App would be asked for access after every update of the App, and deskhub-flash could not share it.
import CryptoKit
import DeskHubCore
import Darwin
import Foundation

public struct UpdateKey: Sendable {
    let key: SecureEnclave.P256.Signing.PrivateKey

    public var publicKeyPEM: String { key.publicKey.pemRepresentation }
    public var fingerprint: String { UpdateManifest.fingerprint(spkiDER: key.publicKey.derRepresentation) }
    public func sign(_ payload: Data) throws -> Data { try key.signature(for: payload).derRepresentation }

    public static var file: URL {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first ?? URL(fileURLWithPath: NSTemporaryDirectory())
        return support.appendingPathComponent("DeskHub/update-key.se")
    }
    public static func load() -> UpdateKey? {
        guard let blob = try? Data(contentsOf: file), let k = try? SecureEnclave.P256.Signing.PrivateKey(dataRepresentation: blob) else { return nil }
        return UpdateKey(key: k)
    }
    public static func loadOrCreate() throws -> UpdateKey {
        if let k = load() { return k }
        guard SecureEnclave.isAvailable else { throw KeyError.noEnclave }
        let k = try SecureEnclave.P256.Signing.PrivateKey()
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        let fd = Darwin.open(file.path, O_WRONLY | O_CREAT | O_EXCL, 0o600)
        guard fd >= 0 else { throw KeyError.notStored }
        let blob = [UInt8](k.dataRepresentation)
        let wrote = Darwin.write(fd, blob, blob.count)
        Darwin.close(fd)
        guard wrote == blob.count else { try? FileManager.default.removeItem(at: file); throw KeyError.notStored }
        return UpdateKey(key: k)
    }
    public enum KeyError: Error { case noEnclave, notStored }

    // The signed envelope for a package: the 8 fields of the app image, signed here.
    public func envelope(for package: FirmwarePackage) throws -> Data {
        guard let app = package.images.first(where: { $0.offset == 0x20000 }) else { throw KeyError.notStored }
        let payload = UpdateManifest.payload(version: package.version, size: app.size, sha256: app.sha256)
        return UpdateManifest.envelope(payload: payload, signatureDER: try sign(payload))
    }
}

extension USBStatus {
    // {"op":"update_key","public_key_pem":...} (13.11, U15). The fingerprint the device stored, or why not.
    public static func writeUpdateKey(port: String, pem: String, seconds: Double = 8) -> Result<String, FlashError> {
        guard let body = try? JSONSerialization.data(withJSONObject: ["op": "update_key", "public_key_pem": pem], options: [.sortedKeys, .withoutEscapingSlashes]) else { return .failure(.noReadBack) }
        return request(port: port, line: body, seconds: seconds) { o in
            if (o["ok"] as? Bool) == false { return .failure(.keyRefused) }
            guard let r = o["result"] as? [String: Any], r["saved"] as? Bool == true, let fp = r["fingerprint"] as? String else { return nil }
            return .success(fp)
        }
    }

    // One request line and the first answer the reader takes (nil: not this answer, keep reading).
    static func request<T>(port: String, line: Data, seconds: Double, answer: ([String: Any]) -> Result<T, FlashError>?) -> Result<T, FlashError> {
        guard !port.isEmpty, !port.unicodeScalars.contains(where: { $0.value < 32 }) else { return .failure(.noPort) }
        let fd = Darwin.open(port, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard fd >= 0 else { return .failure(errno == EBUSY ? .portBusy("串口被占用") : .noReadBack) }
        defer { Darwin.close(fd) }
        var bits: Int32 = 0x2 | 0x4
        _ = ioctl(fd, 0x8004_746B, &bits)
        var t = termios()
        tcgetattr(fd, &t)
        cfmakeraw(&t)
        cfsetspeed(&t, speed_t(B115200))
        t.c_cflag &= ~tcflag_t(HUPCL)
        t.c_cflag |= tcflag_t(CLOCAL | CREAD)
        tcsetattr(fd, TCSANOW, &t)
        let out = [UInt8](line) + [0x0A]
        guard Darwin.write(fd, out, out.count) == out.count else { return .failure(.noReadBack) }
        var reader = LineReader()
        let deadline = Date().addingTimeInterval(seconds)
        var buffer = [UInt8](repeating: 0, count: 4096)
        while Date() < deadline {
            let n = Darwin.read(fd, &buffer, buffer.count)
            if n > 0 {
                for l in reader.feed(Data(buffer[0..<n])) {
                    guard let o = (try? JSONSerialization.jsonObject(with: l)) as? [String: Any], o["ok"] is Bool else { continue }
                    if let r = answer(o) { return r }
                }
                if reader.failed { return .failure(.noReadBack) }
            } else {
                usleep(50_000)
            }
        }
        return .failure(.noReadBack)
    }
}
