// The Bluetooth link to the touch screen (docs/desktop-app-design.md 13; rule K1): CoreBluetooth central. Pairing is done by
// macOS (LE Secure Connections, numeric comparison): reading the encrypted info characteristic makes the Mac ask, and the
// touch screen shows the same six digits. After that the Mac reconnects to that one device by itself.
import CoreBluetooth
import DeskHubCore
import Foundation

final class TouchScreenLink: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum State: Equatable {
        case unavailable(String)   // Bluetooth off, not allowed, not supported
        case notPaired
        case searching             // pairing: looking for a touch screen whose window is open
        case connecting
        case connected(String)     // secure: its name
    }

    static let service = CBUUID(string: "7A5E0001-3C4B-4E0D-9B6A-44534B48ED01")
    static let info = CBUUID(string: "7A5E0002-3C4B-4E0D-9B6A-44534B48ED01")
    static let control = CBUUID(string: "7A5E0003-3C4B-4E0D-9B6A-44534B48ED01")
    static let events = CBUUID(string: "7A5E0004-3C4B-4E0D-9B6A-44534B48ED01")
    private static let rememberKey = "touchScreenPeripheral"

    @Published private(set) var state: State = .notPaired
    @Published private(set) var problem: String?
    var onRequest: ((SwitchRequest) -> Void)?
    var sources: () -> [String] = { [] }   // the messages of the current source list (LinkProtocol.sourceMessages)
    var current: () -> String = { "" }     // the id of the input the monitor is on, "" when not known

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var controlChar: CBCharacteristic?
    private var queue: [Data] = []
    private var writing = false
    private var remembered: UUID? {
        get { UserDefaults.standard.string(forKey: Self.rememberKey).flatMap(UUID.init(uuidString:)) }
        set { UserDefaults.standard.set(newValue?.uuidString, forKey: Self.rememberKey) }
    }

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    var isPaired: Bool { remembered != nil }

    // ---- what the settings window asks ----
    func pair() {
        guard central.state == .poweredOn else { return }
        problem = nil
        state = .searching
        central.scanForPeripherals(withServices: [Self.service])
        DispatchQueue.main.asyncAfter(deadline: .now() + 120) { [weak self] in   // the window on the touch screen is 2 minutes
            guard let self, self.state == .searching else { return }
            self.central.stopScan()
            self.state = .notPaired
            self.problem = "没有找到正在等配对的触摸屏。先在触摸屏上点“配对电脑”。"
        }
    }
    // Forgets the device here. The bond in macOS stays until it is removed in System Settings › Bluetooth.
    func forget() {
        if let p = peripheral { central.cancelPeripheralConnection(p) }
        peripheral = nil
        remembered = nil
        controlChar = nil
        state = .notPaired
    }
    func resendSources() {
        guard case .connected = state else { return }
        for m in sources() { enqueue(m) }
        enqueue(LinkProtocol.currentMessage(current()))
    }
    private var lastCurrent: String?
    func sendCurrent(_ id: String) {
        guard case .connected = state, id != lastCurrent else { return }
        lastCurrent = id
        enqueue(LinkProtocol.currentMessage(id))
    }
    func sendResult(_ message: String) { enqueue(message) }

    // ---- CoreBluetooth ----
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            if let id = remembered, let p = c.retrievePeripherals(withIdentifiers: [id]).first { connect(p) }
            else { state = .notPaired }
        case .unauthorized: state = .unavailable("DeskHub 没有蓝牙权限：在 系统设置 › 隐私与安全性 › 蓝牙 里打开")
        case .poweredOff: state = .unavailable("这台 Mac 的蓝牙关着")
        case .unsupported: state = .unavailable("这台 Mac 不支持低功耗蓝牙")
        default: break
        }
    }
    private func connect(_ p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        state = .connecting
        central.connect(p)   // no timeout: the Mac connects whenever the device comes into range
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
        guard state == .searching else { return }
        c.stopScan()
        connect(p)
    }
    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        p.discoverServices([Self.service])
    }
    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        problem = "连不上触摸屏：\(error?.localizedDescription ?? "未知原因")"
        if remembered != nil { connect(p) } else { state = .notPaired }
    }
    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        controlChar = nil
        queue.removeAll()
        writing = false
        lastCurrent = nil
        if remembered == p.identifier { connect(p) }   // reconnect by itself
        else if state != .searching { state = .notPaired }
    }
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == Self.service }) else { problem = "这个设备没有 DeskHub 的服务"; return }
        p.discoverCharacteristics([Self.info, Self.control, Self.events], for: s)
    }
    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        guard let chars = s.characteristics else { return }
        controlChar = chars.first { $0.uuid == Self.control }
        if let info = chars.first(where: { $0.uuid == Self.info }) { p.readValue(for: info) }   // encrypted: pairs or uses the bond
    }
    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        if ch.uuid == Self.info {
            if let error {
                problem = "配对没有完成：\(error.localizedDescription)"
                if remembered == nil { central.cancelPeripheralConnection(p); state = .notPaired }
                return
            }
            // The link is encrypted and authenticated: remember the device, listen, send the list.
            remembered = p.identifier
            problem = nil
            state = .connected(p.name ?? "DeskHub")
            if let events = p.services?.first?.characteristics?.first(where: { $0.uuid == Self.events }) { p.setNotifyValue(true, for: events) }
            resendSources()
        } else if ch.uuid == Self.events, let v = ch.value, let r = LinkProtocol.parseRequest(v) {
            onRequest?(r)
        }
    }
    func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if let error { problem = "发给触摸屏失败：\(error.localizedDescription)" }
        writing = false
        flush()
    }

    private func enqueue(_ message: String) {
        queue.append(Data(message.utf8))
        flush()
    }
    private func flush() {
        guard !writing, !queue.isEmpty, let p = peripheral, let c = controlChar, case .connected = state else { return }
        writing = true
        p.writeValue(queue.removeFirst(), for: c, type: .withResponse)
    }
}
