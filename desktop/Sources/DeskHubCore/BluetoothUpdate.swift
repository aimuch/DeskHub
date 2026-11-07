// The firmware update over Bluetooth, the App's side (docs/desktop-app-design.md 13.11). The device's side is
// firmware/components/ble_link/ble_update.cpp (frames) and management/update_policy.cpp (the 8 fields): keep them the same.
import CryptoKit
import Foundation

public enum UpdateFrames {
    public static let window = 24 * 1024          // at most this much sent and not yet written by the device
    public static let maxManifest = 4096
    public static let begin = Data([0x42]), end = Data([0x45]), abort = Data([0x58])

    // 'M' + offset (2 bytes, little endian) + bytes; each frame at most `maxFrame` bytes.
    public static func manifestFrames(_ envelope: Data, maxFrame: Int) -> [Data] {
        let room = maxFrame - 3
        guard !envelope.isEmpty, envelope.count <= maxManifest, room > 0 else { return [] }
        let bytes = [UInt8](envelope)
        var out: [Data] = []
        var off = 0
        while off < bytes.count {
            let n = min(room, bytes.count - off)
            out.append(Data([0x4D, UInt8(off & 0xff), UInt8(off >> 8)] + bytes[off..<(off + n)]))
            off += n
        }
        return out
    }
    // 'D' + offset (4 bytes, little endian) + bytes.
    public static func dataFrame(offset: UInt32, _ bytes: Data) -> Data {
        var d = Data([0x44, UInt8(offset & 0xff), UInt8((offset >> 8) & 0xff), UInt8((offset >> 16) & 0xff), UInt8(offset >> 24)])
        d.append(bytes)
        return d
    }
    // How many bytes of data fit in one frame of `maxFrame` bytes.
    public static func dataPerFrame(maxFrame: Int) -> Int { max(0, maxFrame - 5) }
    // May the next frame of `next` bytes go out, with `sent` bytes sent and `written` written by the device?
    public static func maysend(sent: Int, written: Int, next: Int) -> Bool { sent - written + next <= window }
}

public enum UpdateManifest {
    // Exactly the 8 fields of update_policy.cpp.
    public static func payload(version: String, size: Int, sha256: String) -> Data {
        let o: [String: Any] = ["schema_version": 1, "board_model": FirmwareRules.board, "chip": "esp32s3", "version": version,
                                "config_schema_min": 1, "config_schema_max": 1, "image_size": size, "image_sha256": sha256]
        return (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys])) ?? Data()
    }
    // {"algorithm":"ecdsa-p256-sha256","payload":base64,"signature":base64(DER)}
    public static func envelope(payload: Data, signatureDER: Data) -> Data {
        let o: [String: Any] = ["algorithm": "ecdsa-p256-sha256", "payload": payload.base64EncodedString(), "signature": signatureDER.base64EncodedString()]
        return (try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .withoutEscapingSlashes])) ?? Data()
    }
    // The SHA-256 of the SPKI DER of a public key, as the device's update_key_fingerprint writes it.
    public static func fingerprint(spkiDER: Data) -> String { SHA256.hash(data: spkiDER).map { String(format: "%02x", $0) }.joined() }
}

public enum UpdateEvent: Equatable, Sendable {
    case ready
    case writing(Int)
    case refused(String)
    case failed(String)
    case rebooting

    // {"op":"update","state":...}: anything else is not an update event (nil).
    public static func parse(_ data: Data) -> UpdateEvent? {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], o["op"] as? String == "update",
              let state = o["state"] as? String else { return nil }
        switch state {
        case "ready": return .ready
        case "rebooting": return .rebooting
        case "writing":
            guard let w = o["written"], let n = JSONValue.int(w), n >= 0 else { return nil }
            return .writing(n)
        case "refused", "failed":
            guard let e = o["error"] as? String, !e.isEmpty else { return nil }
            return state == "refused" ? .refused(e) : .failed(e)
        default: return nil
        }
    }

    public var words: String {
        switch self {
        case .ready: return "设备已准备好"
        case .writing(let n): return "已写入 \(n) 字节"
        case .rebooting: return "设备正在重启"
        case .refused(let why), .failed(let why): return UpdateEvent.reason(why)
        }
    }
    public static func reason(_ code: String) -> String {
        switch code {
        case "UPDATE_SIGNATURE_INVALID": return "签名不对：设备上的公钥不是这台 Mac 的。先用 USB 写入公钥。"
        case "UPDATE_NOT_NEWER": return "固件包不比设备上的新。蓝牙只能升级；要降级请用 USB。"
        case "UPDATE_TOO_LARGE", "UPDATE_TOO_SMALL": return "镜像大小不对。"
        case "UPDATE_BUSY": return "设备正在做另一次升级。"
        case "UPDATE_SELF_TEST_PENDING": return "设备上一次升级还在自检，等 1 分钟再试。"
        case "UPDATE_POWER_UNSTABLE": return "设备要插着 USB 电源才能升级。"
        case "UPDATE_STORAGE_FAULT": return "设备的存储有问题，请用 USB 刷。"
        case "UPDATE_IMAGE_MISMATCH": return "镜像和清单对不上（不是这块板，或者版本不一样）。"
        case "UPDATE_HASH_MISMATCH": return "收完后校验不符，没有切换。"
        case "UPDATE_IDLE": return "15 秒没收到数据，设备取消了。"
        case "UPDATE_OVERRUN": return "数据来得太快，设备取消了。"
        case "UPDATE_NO_MEMORY": return "设备内存不够，没有开始。"
        case "INVALID_UPDATE_MANIFEST": return "清单不合法。"
        default: return "设备说：\(code)"
        }
    }
}

// What the device says about itself over Bluetooth (the info characteristic).
public struct DeviceInfo: Equatable, Sendable {
    public var device: String
    public var firmware: String
    public var updateKey: String   // fingerprint, "" when none (firmware before 0.3.22 has no such field)
    public init(device: String, firmware: String, updateKey: String) {
        self.device = device
        self.firmware = firmware
        self.updateKey = updateKey
    }
    public static func parse(_ data: Data) -> DeviceInfo? {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let device = o["device"] as? String, let firmware = o["firmware"] as? String else { return nil }
        return DeviceInfo(device: device, firmware: firmware, updateKey: o["update_key"] as? String ?? "")
    }
}
