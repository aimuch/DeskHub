// deskhub-flash: USB flashing from the command line, through the same code as the App's firmware page (Flasher). It writes
// a JSON record of the run, for the evidence.
//   deskhub-flash --port /dev/cu.usbmodem20101 deskhub-0.3.21-esp32s3.zip [--log run.json]
//   deskhub-flash --port /dev/cu.usbmodem20101 --status
//   deskhub-flash --check deskhub-0.3.21-esp32s3.zip
// esptool is found next to this program, in DeskHub.app, or at $DESKHUB_ESPTOOL.
import DeskHubCore
import Flasher
import Foundation

var args = Array(CommandLine.arguments.dropFirst())
func take(_ flag: String) -> String? {
    guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
    let v = args[i + 1]
    args.removeSubrange(i...(i + 1))
    return v
}
func flag(_ f: String) -> Bool {
    guard let i = args.firstIndex(of: f) else { return false }
    args.remove(at: i)
    return true
}
let port = take("--port")
let logPath = take("--log")
let statusOnly = flag("--status"), checkOnly = flag("--check"), toolOnly = flag("--tool")

if toolOnly {   // T1: which esptool runs, and that it says the pinned version
    guard let tool = Esptool.find() else { print(FlashError.tool.words); exit(1) }
    print(tool.executable.path)
    print(tool.version() ? "esptool \(FirmwareRules.esptoolVersion)" : FlashError.tool.words)
    exit(tool.version() ? 0 : 1)
}

if statusOnly {
    guard let port else { print("--status 要和 --port 一起用"); exit(2) }
    switch USBStatus.identify(port: port) {
    case .success(let id): print("\(id.deviceID) \(id.version)"); exit(0)
    case .failure(let e): print(e.words); exit(1)
    }
}
guard let path = args.first else { print("用法见文件开头"); exit(2) }
let folder: PackageFolder
do { folder = try PackageFolder.open(URL(fileURLWithPath: path)) } catch { print(FlashError.notAPackage.words); exit(1) }
defer { folder.remove() }
if checkOnly {
    switch folder.verify() {
    case .success(let p): print("离线核对通过：DeskHub \(p.version)，\(p.images.count) 个镜像，\(p.totalBytes) 字节。没有连设备。"); exit(0)
    case .failure(let e): print(e.words); exit(1)
    }
}
guard let port else { print("要用 --port 指定串口：\(USBStatus.ports().joined(separator: " "))"); exit(2) }
guard let tool = Esptool.find() else { print(FlashError.tool.words); exit(1) }

final class Record: @unchecked Sendable {
    var events: [[String: Any]] = []
    var lastPercent = -1
    let start = Date()
    func add(_ e: [String: Any]) {
        var e = e
        e["t"] = (Date().timeIntervalSince(start) * 10).rounded() / 10
        events.append(e)
    }
}
let record = Record()
let result = FlashRun(package: folder, port: port, tool: tool).run { e in
    switch e {
    case .checking: print("核对固件包…"); record.add(["event": "checking"])
    case .packageOK(let v, let n): print("固件包 \(v)，\(n) 字节，校验都对"); record.add(["event": "package_ok", "version": v, "bytes": n])
    case .before(let id): print("刷之前：\(id.map { "\($0.deviceID) \($0.version)" } ?? "没读到（可能在下载模式）")"); record.add(["event": "before", "device_id": id?.deviceID ?? "", "version": id?.version ?? ""])
    case .probing: print("识别芯片…"); record.add(["event": "probing"])
    case .writing(let f):
        let pct = Int(f * 100)
        if pct / 10 != record.lastPercent / 10 || pct == 100 { print("写入 \(pct)%"); record.add(["event": "writing", "percent": pct]) }
        record.lastPercent = pct
    case .restarting: print("写入和 esptool 校验通过，等设备重启…"); record.add(["event": "restarting"])
    case .confirmed(let id): print("已刷入 \(id.version)（读回确认，\(id.deviceID)）"); record.add(["event": "confirmed", "device_id": id.deviceID, "version": id.version])
    case .log(let l): record.add(["event": "esptool", "line": String(l.prefix(300))])
    }
}
switch result {
case .success: record.add(["event": "done", "ok": true])
case .failure(let e): print(e.words); record.add(["event": "done", "ok": false, "words": e.words])
}
if let logPath {
    let out: [String: Any] = ["tool": "deskhub-flash", "esptool": FirmwareRules.esptoolVersion, "port": port, "package": URL(fileURLWithPath: path).lastPathComponent, "events": record.events]
    if let d = try? JSONSerialization.data(withJSONObject: out, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]) { try? d.write(to: URL(fileURLWithPath: logPath)) }
}
if case .failure = result { exit(1) }
