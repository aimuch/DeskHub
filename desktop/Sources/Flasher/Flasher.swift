// USB flashing of the touch screen (docs/desktop-app-design.md 13.10): the files, the port and the tool. The rules are in
// DeskHubCore/Firmware.swift; this file opens what they check. The App's firmware page and the command line deskhub-flash
// run the same code, so the end-to-end run on the device is the App's path.
import DeskHubCore
import Darwin
import Foundation

public enum FlashError: Error, Equatable, Sendable {
    case notAPackage
    case package(PackageProblem)
    case tool                        // T1
    case noPort                      // D1
    case portBusy(String)            // D2: esptool's own words
    case notThisBoard                // D3
    case packageChanged              // D4
    case writeFailed(String)         // W1
    case noReadBack                  // R1
    case otherVersion(String)        // R2: what was read
    case otherDevice(String)         // R3
    case cancelled
    case keyRefused                  // U15

    public var words: String {
        switch self {
        case .notAPackage: return "不是固件包。请选 GitHub 出的 deskhub-<版本>-esp32s3.zip，或解压后的文件夹。没有连设备。"
        case .package(let p): return p.words
        case .tool: return "App 里的刷机工具（esptool）坏了或版本不对，请重新安装 App。没有连设备。"
        case .noPort: return "没找到设备。请用能传数据的 USB 线连上，再选串口。"
        case .portBusy(let why): return "打不开串口：\(why)。如果开着串口监视器，先关掉再试。没有写入。"
        case .notThisBoard: return "连着的不是这块板（要 ESP32-S3、16 MB Flash）。没有写入。"
        case .packageChanged: return "识别设备时固件包的文件变了。没有写入。"
        case .writeFailed(let why): return "没刷完（\(why)）。设备可能起不来：按住 BOOT 再插线，进下载模式后再刷。"
        case .noReadBack: return "写入和校验都通过了，但 60 秒内没从 USB 读回版本：不算确认。看一下屏幕，再点“再读一次”。"
        case .otherVersion(let v): return "读回的版本是 \(v)，不是包里的：没有生效。"
        case .otherDevice(let id): return "读回的设备编号是 \(id)，和刷之前的不一样：是不是换了一台？"
        case .cancelled: return "已取消，没有写入。"
        case .keyRefused: return "设备没有收下公钥（要 P-256 的 SPKI PEM），原来的公钥保留。"
        }
    }
}

// ---- the package on disk (P1 to P8) ----
public struct PackageFolder: Sendable {
    public let root: URL
    let temporary: URL?

    // A .zip (GitHub's package, or the Actions artifact that holds it) or a folder. A zip is unpacked into a folder of our own.
    public static func open(_ url: URL) throws -> PackageFolder {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else { throw FlashError.notAPackage }
        if isDir.boolValue { return PackageFolder(root: try findRoot(url), temporary: nil) }
        guard url.pathExtension.lowercased() == "zip" else { throw FlashError.notAPackage }
        let temp = FileManager.default.temporaryDirectory.appendingPathComponent("deskhub-package-\(UUID().uuidString)")
        try unzip(url, into: temp)
        // The Actions artifact is a zip that holds the package's zip: one level more.
        if !FileManager.default.fileExists(atPath: temp.appendingPathComponent("manifest.json").path),
           let inner = (try? FileManager.default.contentsOfDirectory(at: temp, includingPropertiesForKeys: nil))?.filter({ $0.pathExtension.lowercased() == "zip" }),
           inner.count == 1 {
            let deeper = temp.appendingPathComponent("package")
            try unzip(inner[0], into: deeper)
            return PackageFolder(root: try findRoot(deeper), temporary: temp)
        }
        return PackageFolder(root: try findRoot(temp), temporary: temp)
    }

    static func unzip(_ zip: URL, into dir: URL) throws {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        p.arguments = ["-x", "-k", zip.path, dir.path]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        try p.run()
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { throw FlashError.notAPackage }
    }

    // manifest.json at the top, or in the only folder at the top.
    static func findRoot(_ dir: URL) throws -> URL {
        if FileManager.default.fileExists(atPath: dir.appendingPathComponent("manifest.json").path) { return dir }
        let items = ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []).filter { !$0.lastPathComponent.hasPrefix(".") && $0.lastPathComponent != "__MACOSX" }
        if items.count == 1, FileManager.default.fileExists(atPath: items[0].appendingPathComponent("manifest.json").path) { return items[0] }
        throw FlashError.notAPackage
    }

    public var manifest: Data? { read("manifest.json", max: FirmwareRules.maxManifest + 1) }

    // A path of the package: no link in any component, inside the root, at most `max` bytes read.
    public func file(_ relative: String) -> PackageFile {
        var current = root
        for part in relative.split(separator: "/") {
            current = current.appendingPathComponent(String(part))
            var st = stat()
            if lstat(current.path, &st) != 0 { return .missing }
            if (st.st_mode & S_IFMT) == S_IFLNK { return .symlink }
        }
        let resolved = current.resolvingSymlinksInPath().standardizedFileURL.path
        guard resolved.hasPrefix(root.resolvingSymlinksInPath().standardizedFileURL.path + "/") else { return .symlink }
        guard let d = read(relative, max: 0x600000 + 1) else { return .missing }
        return .data(d)
    }

    func read(_ relative: String, max: Int) -> Data? {
        guard let h = try? FileHandle(forReadingFrom: root.appendingPathComponent(relative)) else { return nil }
        defer { try? h.close() }
        return try? h.read(upToCount: max) ?? Data()
    }

    public func verify() -> Result<FirmwarePackage, PackageProblem> { FirmwareCheck.verify(manifest: manifest, file: file) }
    public func url(of image: FirmwareImage) -> URL { root.appendingPathComponent(image.path) }
    public func remove() { if let temporary { try? FileManager.default.removeItem(at: temporary) } }
}

// ---- esptool (T1, D2, D3, W1) ----
public struct Esptool: Sendable {
    public let executable: URL

    // In DeskHub.app: Contents/Resources/esptool/esptool. For the command line: next to it, or DESKHUB_ESPTOOL.
    // An App installed from the DMG carries the "downloaded" mark on every file, and macOS then stops the bundled program
    // with a question nobody sees (2026-11-07). So the App runs its own copy: the bytes written anew into Application
    // Support (no marks), checked against the SHA-256 of the release.
    public static func find() -> Esptool? {
        var candidates: [URL] = []
        if let env = ProcessInfo.processInfo.environment["DESKHUB_ESPTOOL"] { candidates.append(URL(fileURLWithPath: env)) }
        if let r = Bundle.main.resourceURL { candidates.append(r.appendingPathComponent("esptool/esptool")) }
        candidates.append(URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent().appendingPathComponent("esptool"))
        guard let found = candidates.first(where: { FileManager.default.isExecutableFile(atPath: $0.path) }) else { return nil }
        return Esptool(executable: privateCopy(of: found) ?? found)
    }

    static func privateCopy(of source: URL) -> URL? {
        guard let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else { return nil }
        let dir = support.appendingPathComponent("DeskHub/esptool-\(FirmwareRules.esptoolVersion)")
        let copy = dir.appendingPathComponent("esptool")
        if let d = try? Data(contentsOf: copy), FirmwareCheck.sha256HexForTests(d) == FirmwareRules.esptoolSHA256 { return copy }
        guard let bytes = try? Data(contentsOf: source), FirmwareCheck.sha256HexForTests(bytes) == FirmwareRules.esptoolSHA256 else { return nil }
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try? FileManager.default.removeItem(at: copy)
            try bytes.write(to: copy, options: .atomic)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: copy.path)
            return copy
        } catch {
            return nil
        }
    }

    // Runs it, line by line ("\r" counts as a new line: the progress). `timeout` nil: never killed (a write in progress).
    @discardableResult
    public func run(_ args: [String], timeout: TimeInterval?, line: @escaping @Sendable (String) -> Void = { _ in }) -> (status: Int32, output: String) {
        let p = Process()
        p.executableURL = executable
        p.arguments = args
        p.currentDirectoryURL = FileManager.default.temporaryDirectory   // PyInstaller unpacks next to nothing of ours
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        let collected = Collected()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            guard !d.isEmpty else { return }
            for l in collected.add(d) { line(l) }
        }
        do { try p.run() } catch { return (-1, "") }
        let done = DispatchSemaphore(value: 0)
        p.terminationHandler = { _ in done.signal() }
        if let timeout, done.wait(timeout: .now() + timeout) == .timedOut {
            p.terminate()
            done.wait()
        } else if timeout == nil {
            done.wait()
        }
        pipe.fileHandleForReading.readabilityHandler = nil
        let rest = pipe.fileHandleForReading.readDataToEndOfFile()
        for l in collected.add(rest) + collected.flush() { line(l) }
        return (p.terminationStatus, collected.all)
    }

    public func version() -> Bool { EsptoolOutput.versionOK(run(["version"], timeout: 30).output) }
}

final class Collected: @unchecked Sendable {
    private let lock = NSLock()
    private var partial = "", text = ""
    func add(_ d: Data) -> [String] {
        lock.lock(); defer { lock.unlock() }
        let s = String(decoding: d, as: UTF8.self)
        text += s
        if text.count > 1_000_000 { text = String(text.suffix(500_000)) }
        partial += s
        var lines: [String] = []
        while let r = partial.firstIndex(where: { $0 == "\n" || $0 == "\r" }) {
            lines.append(String(partial[..<r]))
            partial = String(partial[partial.index(after: r)...])
        }
        return lines.filter { !$0.isEmpty }
    }
    func flush() -> [String] { lock.lock(); defer { lock.unlock() }; let p = partial; partial = ""; return p.isEmpty ? [] : [p] }
    var all: String { lock.lock(); defer { lock.unlock() }; return text }
}

// ---- the USB status of the running firmware (R1 to R4) ----
public enum USBStatus {
    // The serial ports a DeskHub can be on. The person picks one: never the first one by itself.
    public static func ports() -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []).filter { $0.hasPrefix("cu.usbmodem") }.sorted().map { "/dev/" + $0 }
    }

    // {"op":"identify"} and its answer, within `seconds`. The control lines are cleared at once: no reset on purpose.
    public static func identify(port: String, seconds: Double = 8) -> Result<DeviceIdentity, FlashError> {
        guard !port.isEmpty, !port.unicodeScalars.contains(where: { $0.value < 32 }) else { return .failure(.noPort) }
        let fd = Darwin.open(port, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard fd >= 0 else { return .failure(errno == EBUSY ? .portBusy("串口被占用") : .noReadBack) }
        defer { Darwin.close(fd) }
        var bits: Int32 = 0x2 | 0x4                       // TIOCM_DTR | TIOCM_RTS
        _ = ioctl(fd, 0x8004_746B, &bits)                 // TIOCMBIC: both low
        var t = termios()
        tcgetattr(fd, &t)
        cfmakeraw(&t)
        cfsetspeed(&t, speed_t(B115200))
        t.c_cflag &= ~tcflag_t(HUPCL)
        t.c_cflag |= tcflag_t(CLOCAL | CREAD)
        tcsetattr(fd, TCSANOW, &t)
        let request = Array(#"{"op":"identify"}"#.utf8) + [0x0A]
        guard write(fd, request, request.count) == request.count else { return .failure(.noReadBack) }
        var reader = LineReader()
        let deadline = Date().addingTimeInterval(seconds)
        var buffer = [UInt8](repeating: 0, count: 4096)
        while Date() < deadline {
            let n = read(fd, &buffer, buffer.count)
            if n > 0 {
                for l in reader.feed(Data(buffer[0..<n])) {
                    switch StatusParse.line(l) {
                    case .skip: continue
                    case .refused, .bad: return .failure(.noReadBack)
                    case .identity(let id): return .success(id)
                    }
                }
                if reader.failed { return .failure(.noReadBack) }
            } else {
                usleep(50_000)
            }
        }
        return .failure(.noReadBack)
    }
}

// ---- the whole flash (13.10) ----
public enum FlashEvent: Sendable, Equatable {
    case checking                      // the package
    case packageOK(String, Int)        // version, bytes
    case before(DeviceIdentity?)       // what the running firmware said before (nil: no answer, maybe in download mode)
    case probing
    case writing(Double)               // 0...1
    case restarting                    // written and verified by esptool; waiting for the firmware
    case confirmed(DeviceIdentity)     // K3: read back, the same version
    case log(String)                   // esptool's line, for the record
}

public struct FlashRun: Sendable {
    public let package: PackageFolder
    public let port: String
    public let tool: Esptool
    public init(package: PackageFolder, port: String, tool: Esptool) {
        self.package = package
        self.port = port
        self.tool = tool
    }

    // Blocking: run it off the main thread. `isCancelled` is asked only before the write starts.
    public func run(isCancelled: @Sendable () -> Bool = { false }, event: @escaping @Sendable (FlashEvent) -> Void) -> Result<DeviceIdentity, FlashError> {
        event(.checking)
        let pkg: FirmwarePackage
        switch package.verify() {
        case .failure(let p): return .failure(.package(p))
        case .success(let p): pkg = p
        }
        event(.packageOK(pkg.version, pkg.totalBytes))
        guard tool.version() else { return .failure(.tool) }
        guard !port.isEmpty, FileManager.default.fileExists(atPath: port) else { return .failure(.noPort) }
        let before = try? USBStatus.identify(port: port).get()
        event(.before(before))
        if isCancelled() { return .failure(.cancelled) }

        event(.probing)
        let base = ["--chip", "esp32s3", "--port", port, "--no-stub"]
        let probe = tool.run(base + ["flash-id"], timeout: 45) { event(.log($0)) }
        if probe.status != 0 {
            let why = probe.output.split(whereSeparator: \.isNewline).last { $0.contains("error") || $0.contains("Error") }.map(String.init) ?? "esptool 退出码 \(probe.status)"
            return .failure(probe.output.contains("Could not open") || probe.output.contains("Resource busy") ? .portBusy(why) : .notThisBoard)
        }
        guard EsptoolOutput.probeOK(probe.output) else { return .failure(.notThisBoard) }
        guard case .success(let again) = package.verify(), again == pkg else { return .failure(.packageChanged) }
        if isCancelled() { return .failure(.cancelled) }

        var args = base + ["write-flash", "--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"]
        for img in pkg.images { args += [String(format: "0x%x", img.offset), package.url(of: img).path] }
        event(.writing(0))
        let write = tool.run(args, timeout: nil) { l in
            if let f = EsptoolOutput.progress(l, package: pkg) { event(.writing(f)) } else { event(.log(l)) }
        }
        guard write.status == 0, write.output.contains("Hash of data verified") else {
            let why = write.output.split(whereSeparator: \.isNewline).last { $0.lowercased().contains("error") }.map(String.init) ?? "esptool 退出码 \(write.status)"
            return .failure(.writeFailed(why))
        }
        event(.writing(1))
        event(.restarting)
        // R1 to R3: the firmware takes some seconds to start; the first read may also reset it once.
        Thread.sleep(forTimeInterval: 12)
        let deadline = Date().addingTimeInterval(60)
        while Date() < deadline {
            if case .success(let id) = USBStatus.identify(port: port) {
                if let before, before.deviceID != id.deviceID { return .failure(.otherDevice(id.deviceID)) }
                guard id.version == pkg.version else { return .failure(.otherVersion(id.version)) }
                event(.confirmed(id))
                return .success(id)
            }
            Thread.sleep(forTimeInterval: 4)
        }
        return .failure(.noReadBack)
    }
}
