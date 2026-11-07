// USB flashing of the touch screen (docs/desktop-app-design.md 13.10): the package check, the reading of the USB status,
// and the reading of esptool's output. Pure: the App hands in the bytes; nothing here opens a file, a port or a process.
// The rules are those of the package's own flash.py (scripts/package_firmware.py writes both).
import CryptoKit
import Foundation

public enum FirmwareRules {
    public static let board = "ESP32-S3-Touch-AMOLED-2.16"
    public static let flashBytes = 16 * 1024 * 1024
    public static let maxManifest = 65536
    public static let esptoolVersion = "5.6.0"
    // The esptool program inside the release asset (desktop/scripts/fetch_esptool.sh checks the asset itself).
    public static let esptoolSHA256 = "fe300417054386e8c15f916216924accb05d0132411bfce3b223ad621f0de6d9"
    // The fixed partition layout: offset, path, the most it may hold. NVS and PHY data in the gaps are never written.
    public static let layout: [(offset: UInt32, path: String, max: Int)] = [
        (0x0, "bootloader/bootloader.bin", 0x8000),
        (0x8000, "partition_table/partition-table.bin", 0x1000),
        (0xF000, "ota_data_initial.bin", 0x2000),
        (0x20000, "deskhub.bin", 0x600000),
    ]
}

public struct FirmwareImage: Equatable, Sendable {
    public var offset: UInt32
    public var path: String
    public var size: Int
    public var sha256: String
    public init(offset: UInt32, path: String, size: Int, sha256: String) {
        self.offset = offset
        self.path = path
        self.size = size
        self.sha256 = sha256
    }
}

public struct FirmwarePackage: Equatable, Sendable {
    public var version: String
    public var images: [FirmwareImage]   // sorted by offset
    public init(version: String, images: [FirmwareImage]) {
        self.version = version
        self.images = images
    }
    public var totalBytes: Int { images.reduce(0) { $0 + $1.size } }
}

// What the App finds at a path of the package (relative, "/"-separated).
public enum PackageFile: Equatable, Sendable {
    case missing
    case symlink          // any component of the path is a link
    case data(Data)
}

public enum PackageProblem: Error, Equatable, Sendable {
    case manifestMissing, manifestBad, wrongSchema, wrongBoard, badVersion, wrongFlashSettings, wrongImageCount
    case badPath, symlink, badOffset, duplicate, tooLarge, overlap, badDigest
    case missingFile, sizeMismatch, digestMismatch, notESP32S3, badDescriptor, versionMismatch

    public var words: String {
        switch self {
        case .manifestMissing, .manifestBad: return "包清单不存在或不合法。没有连设备。"
        case .wrongSchema, .wrongBoard: return "这不是这块板的 DeskHub 固件包。没有连设备。"
        case .badVersion: return "固件版本号不合法。没有连设备。"
        case .wrongFlashSettings: return "包的 Flash 参数和这块板不一样。没有连设备。"
        case .wrongImageCount: return "固件包要正好有 4 个镜像。没有连设备。"
        case .badPath, .symlink: return "镜像路径不合法。没有连设备。"
        case .badOffset, .duplicate, .tooLarge, .overlap: return "镜像不符合这块板的固定分区。没有连设备。"
        case .badDigest: return "镜像的校验信息不合法。没有连设备。"
        case .missingFile, .sizeMismatch: return "镜像缺失或大小不对，请重新下载。没有连设备。"
        case .digestMismatch: return "镜像校验不符，请重新下载。没有连设备。"
        case .notESP32S3: return "镜像不是 ESP32-S3 的。没有连设备。"
        case .badDescriptor, .versionMismatch: return "应用镜像的项目或版本和清单不一样。没有连设备。"
        }
    }
}

public enum FirmwareCheck {
    // P1 to P8: the whole package is checked before a port is opened or a tool runs.
    public static func verify(manifest: Data?, file: (String) -> PackageFile) -> Result<FirmwarePackage, PackageProblem> {
        guard let manifest else { return .failure(.manifestMissing) }
        guard manifest.count <= FirmwareRules.maxManifest,
              let m = (try? JSONSerialization.jsonObject(with: manifest)) as? [String: Any] else { return .failure(.manifestBad) }
        guard let schema = m["schema_version"], JSONValue.int(schema) == 1 else { return .failure(.wrongSchema) }
        guard m["board"] as? String == FirmwareRules.board, m["chip"] as? String == "esp32s3",
              let signed = m["signed_ota"], JSONValue.isBool(signed), (signed as? Bool) == false else { return .failure(.wrongBoard) }
        guard let version = m["version"] as? String, JSONValue.safeVersion(version) else { return .failure(.badVersion) }
        guard let fs = m["flash_settings"] as? [String: Any], fs.count == 3, fs["flash_mode"] as? String == "dio",
              fs["flash_size"] as? String == "16MB", fs["flash_freq"] as? String == "80m" else { return .failure(.wrongFlashSettings) }
        guard let items = m["files"] as? [Any], items.count == FirmwareRules.layout.count else { return .failure(.wrongImageCount) }

        // First the form of every path and offset, then repeats, then each image against the layout and its bytes.
        var entries: [(item: [String: Any], path: String, offset: UInt32)] = []
        for any in items {
            guard let item = any as? [String: Any] else { return .failure(.manifestBad) }
            guard let path = item["path"] as? String, validPath(path) else { return .failure(.badPath) }
            guard let text = item["offset"] as? String, text.hasPrefix("0x"), text.count > 2, text.count <= 10,
                  text.dropFirst(2).allSatisfy(\.isHexDigit), let offset = UInt32(text.dropFirst(2), radix: 16) else { return .failure(.badOffset) }
            entries.append((item, path, offset))
        }
        if Set(entries.map(\.path)).count != entries.count || Set(entries.map(\.offset)).count != entries.count { return .failure(.duplicate) }

        var images: [FirmwareImage] = []
        for e in entries {
            guard let sizeAny = e.item["size_bytes"], let size = JSONValue.int(sizeAny), size > 0,
                  Int(e.offset) + size <= FirmwareRules.flashBytes else { return .failure(.tooLarge) }
            guard let digest = e.item["sha256"] as? String, digest.count == 64,
                  digest.unicodeScalars.allSatisfy({ ("0"..."9").contains($0) || ("a"..."f").contains($0) }) else { return .failure(.badDigest) }
            guard let slot = FirmwareRules.layout.first(where: { $0.offset == e.offset }), slot.path == e.path else { return .failure(.badOffset) }
            guard size <= slot.max else { return .failure(.tooLarge) }
            let data: [UInt8]
            switch file(e.path) {
            case .missing: return .failure(.missingFile)
            case .symlink: return .failure(.symlink)
            case .data(let d): data = [UInt8](d)
            }
            guard data.count == size else { return .failure(.sizeMismatch) }
            guard sha256Hex(Data(data)) == digest else { return .failure(.digestMismatch) }
            if e.offset == 0x0 || e.offset == 0x20000 {
                guard data.count >= 24, data[0] == 0xE9, UInt16(data[12]) | UInt16(data[13]) << 8 == 9 else { return .failure(.notESP32S3) }
            }
            if e.offset == 0x20000 {
                guard data.count >= 112 else { return .failure(.badDescriptor) }
                let magic = UInt32(data[32]) | UInt32(data[33]) << 8 | UInt32(data[34]) << 16 | UInt32(data[35]) << 24
                guard magic == 0xABCD5432, cString(data[80..<112]) == "deskhub" else { return .failure(.badDescriptor) }
                guard cString(data[48..<80]) == version else { return .failure(.versionMismatch) }
            }
            images.append(FirmwareImage(offset: e.offset, path: e.path, size: size, sha256: digest))
        }
        images.sort { $0.offset < $1.offset }
        for (a, b) in zip(images, images.dropFirst()) where Int(a.offset) + a.size > Int(b.offset) { return .failure(.overlap) }
        return .success(FirmwarePackage(version: version, images: images))
    }

    static func validPath(_ p: String) -> Bool {
        if p.isEmpty || p.hasPrefix("/") || p.contains("\\") || p.contains(":") { return false }
        return p.split(separator: "/", omittingEmptySubsequences: false).allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." }
    }
    static func cString(_ bytes: ArraySlice<UInt8>) -> String {
        String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
    }
    static func sha256Hex(_ d: Data) -> String { SHA256.hash(data: d).map { String(format: "%02x", $0) }.joined() }
    public static func sha256HexForTests(_ d: Data) -> String { sha256Hex(d) }
}

// JSON as Python's json reads it: true is not 1, 1.0 is not an integer.
enum JSONValue {
    static func isBool(_ v: Any) -> Bool {
        guard let n = v as? NSNumber else { return false }
        return CFGetTypeID(n) == CFBooleanGetTypeID()
    }
    static func int(_ v: Any) -> Int? {
        guard let n = v as? NSNumber, !isBool(n) else { return nil }
        let type = String(cString: n.objCType)
        guard type != "d" && type != "f" else { return nil }
        return n.intValue
    }
    static func safeVersion(_ s: String) -> Bool {
        (1...31).contains(s.utf8.count) && s.unicodeScalars.allSatisfy { $0.isASCII && (CharacterSet.alphanumerics.contains($0) || "_.+-".unicodeScalars.contains($0)) }
    }
}

// ---- versions ----
public enum FirmwareVersion {
    // -1, 0, 1; nil when either is not dotted numbers (then the App cannot say which is newer)
    public static func compare(_ a: String, _ b: String) -> Int? {
        func parts(_ s: String) -> [Int]? {
            let p = s.split(separator: ".", omittingEmptySubsequences: false)
            guard !p.isEmpty, p.allSatisfy({ !$0.isEmpty && $0.count <= 9 && $0.allSatisfy(\.isASCII) && $0.allSatisfy(\.isNumber) }) else { return nil }
            return p.compactMap { Int($0) }
        }
        guard var x = parts(a), var y = parts(b) else { return nil }
        let n = max(x.count, y.count)
        x += Array(repeating: 0, count: n - x.count)
        y += Array(repeating: 0, count: n - y.count)
        for (i, j) in zip(x, y) where i != j { return i < j ? -1 : 1 }
        return 0
    }
}

// ---- the USB status (R1 to R4) ----
public struct DeviceIdentity: Equatable, Sendable {
    public var deviceID: String
    public var version: String
    public init(deviceID: String, version: String) {
        self.deviceID = deviceID
        self.version = version
    }
}

public enum StatusLine: Equatable, Sendable {
    case skip                    // a log line, an echo, another JSON: not an answer
    case refused                 // ok:false
    case identity(DeviceIdentity)
    case bad(String)             // an answer whose identity is not valid: why
}

public enum StatusParse {
    public static let maxFrame = 16 * 1024, maxTotal = 128 * 1024
    public static func line(_ bytes: Data) -> StatusLine {
        guard let object = try? JSONSerialization.jsonObject(with: bytes), let o = object as? [String: Any],
              let ok = o["ok"], JSONValue.isBool(ok) else { return .skip }
        if (ok as? Bool) == false { return .refused }
        guard let r = o["result"] as? [String: Any],
              ["device_id", "firmware_version", "protocol_version", "board_model"].contains(where: { r[$0] != nil }) else { return .skip }
        guard let id = r["device_id"] as? String, id.utf8.count == 20, id.hasPrefix("deskhub-"),
              id.dropFirst(8).unicodeScalars.allSatisfy({ ("0"..."9").contains($0) || ("a"..."f").contains($0) }) else { return .bad("设备编号不合法") }
        guard r["board_model"] as? String == FirmwareRules.board else { return .bad("设备型号不对") }
        guard let proto = r["protocol_version"] as? String, ["1", "1.0", "1.0.0"].contains(proto) else { return .bad("设备协议不兼容") }
        guard let version = r["firmware_version"] as? String, JSONValue.safeVersion(version) else { return .bad("固件版本不合法") }
        return .identity(DeviceIdentity(deviceID: id, version: version))
    }
}

// Lines out of a byte stream, with the limits of flash.py: a frame over 16 KB or more than 128 KB in all is an error.
public struct LineReader: Sendable {
    public private(set) var failed = false
    private var buffer = Data(), total = 0
    public init() {}
    public mutating func feed(_ bytes: Data) -> [Data] {
        guard !failed else { return [] }
        total += bytes.count
        if total > StatusParse.maxTotal { failed = true; return [] }
        buffer.append(bytes)
        var out: [Data] = []
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = buffer[buffer.startIndex...nl]
            if line.count > StatusParse.maxFrame { failed = true; return out }
            out.append(Data(line))
            buffer = Data(buffer[buffer.index(after: nl)...])
        }
        if buffer.count > StatusParse.maxFrame { failed = true }
        return out
    }
}

// ---- esptool ----
public enum EsptoolOutput {
    public static func versionOK(_ output: String) -> Bool {
        output.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }.last(where: { !$0.isEmpty }) == FirmwareRules.esptoolVersion
    }
    // flash-id: "Chip type: ESP32-S3 ..." and "Detected flash size: 16MB"
    public static func probeOK(_ output: String) -> Bool {
        output.range(of: #"Chip type:\s*ESP32-S3\b"#, options: .regularExpression) != nil &&
            output.range(of: #"Detected flash size:\s*16\s*MB\b"#, options: .regularExpression) != nil
    }
    // The fraction of the whole package written, 0...1, from a progress line of the image being written:
    //   older esptool: "Writing at 0x00463000 [...] 62.0% 2768896/4468752 bytes..."
    //   esptool 5.6.0: "Writing at 0x0045e000 ━━━━━╸ 99.6% 4.24MB/4.26MB [45s]" (megabytes rounded: the percent counts)
    public static func progress(_ line: String, package: FirmwarePackage) -> Double? {
        let range = NSRange(line.startIndex..., in: line)
        guard let at = try? NSRegularExpression(pattern: #"Writing at 0x([0-9a-fA-F]+)\b"#),
              let m = at.firstMatch(in: line, range: range), let a = Range(m.range(at: 1), in: line),
              let address = UInt32(line[a], radix: 16) else { return nil }
        // The image whose partition holds the address; or, at the end of an image, the address just after it (esptool says
        // "Writing at 0x00011000 ... 100%" for the 8 KB at 0xf000).
        let inside = package.images.first { img in
            let slot = FirmwareRules.layout.first { $0.offset == img.offset }
            return address >= img.offset && Int(address) < Int(img.offset) + (slot?.max ?? img.size)
        }
        guard let image = inside ?? package.images.first(where: { Int($0.offset) + $0.size == Int(address) }), package.totalBytes > 0 else { return nil }
        var written: Double?
        if let bytes = try? NSRegularExpression(pattern: #"\b(\d+)/(\d+) bytes"#), let b = bytes.firstMatch(in: line, range: range),
           let w = Range(b.range(at: 1), in: line), let n = Double(line[w]) {
            written = n
        } else if let pct = try? NSRegularExpression(pattern: #"\s(\d{1,3}(?:\.\d+)?)%"#), let p = pct.firstMatch(in: line, range: range),
                  let r = Range(p.range(at: 1), in: line), let v = Double(line[r]) {
            written = Double(image.size) * v / 100
        }
        guard let written else { return nil }
        let before = package.images.filter { $0.offset < image.offset }.reduce(0) { $0 + $1.size }
        return min(1, (Double(before) + min(written, Double(image.size))) / Double(package.totalBytes))
    }
}
