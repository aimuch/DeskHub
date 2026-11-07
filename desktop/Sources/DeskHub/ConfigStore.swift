// Where the App keeps its configuration: ~/Library/Application Support/DeskHub/config.json. A damaged file is moved aside
// (kept, never overwritten) and the App starts empty, saying why (loadConfig: never half a configuration).
import DeskHubCore
import Foundation

struct ConfigStore {
    let folder: URL
    var file: URL { folder.appendingPathComponent("config.json") }

    init(folder: URL? = nil) {
        if let folder { self.folder = folder; return }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first ?? URL(fileURLWithPath: NSHomeDirectory())
        self.folder = base.appendingPathComponent("DeskHub", isDirectory: true)
    }

    func load() -> (AppConfig, String?) {
        let data = (try? Data(contentsOf: file)) ?? Data()
        let (config, problem) = loadConfig(data)
        if problem != nil {
            let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
            try? FileManager.default.moveItem(at: file, to: folder.appendingPathComponent("config.damaged-\(stamp).json"))
        }
        return (config, problem)
    }

    func save(_ config: AppConfig) throws {
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try encodeConfig(config).write(to: file, options: .atomic)
    }
}
