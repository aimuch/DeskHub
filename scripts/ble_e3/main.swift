// Experiment E3 (docs/desktop-app-design.md 13.5): the Mac side. Test tool only, not the App.
// Finds "DeskHub-probe", connects, makes macOS pair (the echo characteristic needs an encrypted, authenticated link), then
// writes 100 bytes every 100 ms and times the notification that comes back. Halfway it disconnects and connects again, to
// see that the bond holds. The clock of the test starts at the first echo, after pairing: waiting for the Bluetooth
// permission or for the pairing dialog does not count. One JSON line per event or per 10 s summary goes to the output file.
//
// Run through `open` (scripts/ble_e3/run.sh), not the binary itself: macOS gives Bluetooth to the app bundle that asks for it.
import CoreBluetooth
import Foundation

let args = CommandLine.arguments
func arg(_ key: String, _ fallback: String) -> String {
    args.first { $0.hasPrefix(key + "=") }.map { String($0.dropFirst(key.count + 1)) } ?? fallback
}
let outPath = arg("out", NSHomeDirectory() + "/deskhub-e3.jsonl")
let seconds = Double(arg("seconds", "600")) ?? 600
let reconnectAt = Double(arg("reconnect", "300")) ?? 300

// The probe's 128-bit UUIDs (firmware/components/probe/probe.c; NimBLE writes them little-endian).
let serviceID = CBUUID(string: "424F5250-4844-218E-9C4B-6D521E7B0A5F")
let heapID = CBUUID(string: "41454850-4844-218E-9C4B-6D521E7B0A5F")
let echoID = CBUUID(string: "4F484345-4844-218E-9C4B-6D521E7B0A5F")

FileManager.default.createFile(atPath: outPath, contents: nil)
let out = FileHandle(forWritingAtPath: outPath)!
let start = Date()
func log(_ fields: [String: Any]) {
    var f = fields
    f["t"] = (Date().timeIntervalSince(start) * 1000).rounded() / 1000
    if let data = try? JSONSerialization.data(withJSONObject: f, options: [.sortedKeys]) {
        out.write(data)
        out.write("\n".data(using: .utf8)!)
    }
}

final class E3: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var peripheral: CBPeripheral?
    var echo: CBCharacteristic?
    var heap: CBCharacteristic?
    var seq: UInt32 = 0
    var sent: [UInt32: Date] = [:]
    var rtts: [Double] = []
    var allRtts: [Double] = []
    var writeErrors = 0, lost = 0, connects = 0, disconnects = 0
    var reconnected = false, wantDisconnect = false
    var timer: Timer?
    var firstEcho: Date?   // the test clock: starts when the link works

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        log(["event": "state", "value": c.state.rawValue])
        if c.state == .poweredOn { scan() }
        if c.state == .unauthorized { log(["event": "fatal", "why": "bluetooth not allowed for this app"]); finish(1) }
    }
    func scan() {
        log(["event": "scan"])
        central.scanForPeripherals(withServices: nil, options: nil)
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
        let name = p.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? ""
        guard name == "DeskHub-probe" else { return }
        c.stopScan()
        log(["event": "found", "rssi": rssi.intValue])
        peripheral = p
        p.delegate = self
        c.connect(p, options: nil)
    }
    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        connects += 1
        log(["event": "connected", "count": connects])
        p.discoverServices([serviceID])
    }
    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log(["event": "connect_failed", "error": error?.localizedDescription ?? ""])
        scan()
    }
    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        disconnects += 1
        echo = nil
        log(["event": "disconnected", "asked": wantDisconnect, "error": error?.localizedDescription ?? ""])
        lost += sent.count
        sent.removeAll()
        let again = wantDisconnect
        wantDisconnect = false
        DispatchQueue.main.asyncAfter(deadline: .now() + (again ? 3 : 1)) { c.connect(p, options: nil) }
    }
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == serviceID }) else { log(["event": "fatal", "why": "no service"]); finish(1); return }
        p.discoverCharacteristics([heapID, echoID], for: s)
    }
    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for ch in s.characteristics ?? [] {
            if ch.uuid == echoID { echo = ch }
            if ch.uuid == heapID { heap = ch }
        }
        guard let e = echo else { log(["event": "fatal", "why": "no echo characteristic"]); finish(1); return }
        p.setNotifyValue(true, for: e)
        if let h = heap { p.readValue(for: h) }   // encrypted: this makes macOS pair, or use the bond
    }
    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {
        log(["event": "notify", "on": ch.isNotifying, "error": error?.localizedDescription ?? ""])
        if ch.isNotifying && timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in self?.tick() }
        }
    }
    func tick() {
        // An echo that has not come back in 2 s is lost; it must not block the next writes.
        let now = Date()
        for (n, at) in sent where now.timeIntervalSince(at) > 2 { sent.removeValue(forKey: n); lost += 1 }
        let elapsed = firstEcho.map { now.timeIntervalSince($0) } ?? 0
        if firstEcho != nil && elapsed >= seconds { summary(); finish(0); return }
        if !reconnected && elapsed >= reconnectAt, let p = peripheral, echo != nil {
            reconnected = true
            wantDisconnect = true
            log(["event": "disconnect_on_purpose"])
            central.cancelPeripheralConnection(p)
            return
        }
        guard let p = peripheral, let e = echo else { return }
        if sent.count >= 3 { return }   // do not pile up: the device answers one by one
        seq += 1
        var payload = Data(count: 100)
        withUnsafeBytes(of: seq.littleEndian) { payload.replaceSubrange(0..<4, with: $0) }
        sent[seq] = Date()
        p.writeValue(payload, for: e, type: .withResponse)
        if seq % 100 == 0 { summary() }
        if seq % 50 == 0, let h = heap { p.readValue(for: h) }
    }
    func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if let error = error { writeErrors += 1; log(["event": "write_error", "error": error.localizedDescription]) }
    }
    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        if let error = error { log(["event": "read_error", "uuid": ch.uuid.uuidString, "error": error.localizedDescription]); return }
        guard let v = ch.value else { return }
        if ch.uuid == heapID {
            log(["event": "device_heap", "internal_free": String(data: v, encoding: .utf8) ?? "?"])
        } else if ch.uuid == echoID, v.count >= 4 {
            let n = v.prefix(4).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }.littleEndian
            if let at = sent.removeValue(forKey: n) {
                if firstEcho == nil { firstEcho = Date(); log(["event": "first_echo"]) }
                let ms = Date().timeIntervalSince(at) * 1000
                rtts.append(ms)
                allRtts.append(ms)
            }
        }
    }
    func median(_ v: [Double]) -> Double { v.isEmpty ? -1 : v.sorted()[v.count / 2] }
    func p95(_ v: [Double]) -> Double { v.isEmpty ? -1 : v.sorted()[min(v.count - 1, Int(Double(v.count) * 0.95))] }
    func summary() {
        log(["event": "summary", "echoes": rtts.count, "median_ms": median(rtts).rounded(), "p95_ms": p95(rtts).rounded(),
             "total_echoes": allRtts.count, "total_median_ms": median(allRtts).rounded(), "write_errors": writeErrors, "lost": lost,
             "connects": connects, "disconnects": disconnects])
        rtts.removeAll()
    }
    func finish(_ code: Int32) {
        timer?.invalidate()
        log(["event": "end", "code": Int(code)])
        out.closeFile()
        exit(code)
    }
}

let test = E3()
DispatchQueue.main.asyncAfter(deadline: .now() + seconds + 900) { log(["event": "fatal", "why": "timeout"]); test.finish(2) }
RunLoop.main.run()
