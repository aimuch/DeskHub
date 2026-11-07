// Every switch and every trial, one JSON line each, in ~/Library/Logs/DeskHub/switch.jsonl: the evidence of what was sent
// and what the monitor read back (K3). Names and values only: nothing secret goes here.
import DeskHubCore
import Foundation

enum SwitchLog {
    static let file: URL = {
        let logs = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).first!.appendingPathComponent("Logs/DeskHub", isDirectory: true)
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        return logs.appendingPathComponent("switch.jsonl")
    }()

    static func write(_ fields: [String: Any], _ r: SwitchResult) {
        var f = fields
        f["time"] = ISO8601DateFormatter().string(from: Date())
        f["outcome"] = r.outcome.rawValue
        f["detail"] = r.detail
        if let v = r.readBack { f["read_back"] = Int(v) }
        guard var data = try? JSONSerialization.data(withJSONObject: f, options: [.sortedKeys]) else { return }
        data.append(0x0A)
        if let h = try? FileHandle(forWritingTo: file) {
            h.seekToEndOfFile()
            h.write(data)
            try? h.close()
        } else {
            try? data.write(to: file)
        }
    }
}
