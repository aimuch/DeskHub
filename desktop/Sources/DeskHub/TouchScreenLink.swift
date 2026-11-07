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
    static let update = CBUUID(string: "7A5E0005-3C4B-4E0D-9B6A-44534B48ED01")   // 13.11, firmware 0.3.22 and later
    private static let rememberKey = "touchScreenPeripheral"

    @Published private(set) var state: State = .notPaired
    @Published private(set) var problem: String?
    @Published private(set) var info: DeviceInfo?           // what the device said about itself at the last connection
    // ---- the firmware update over Bluetooth (docs/desktop-app-design.md 13.11) ----
    enum UpdatePhase: Equatable {
        case idle, manifest, waitingReady, sending(Double), finishing, restarting(String)
        case confirmed(String), failed(String)
    }
    @Published private(set) var update: UpdatePhase = .idle
    var hasUpdateCharacteristic: Bool { updateChar != nil }
    var onUpdateLog: ((String) -> Void)?                     // the record of a run (the test hook writes it)
    var onRequest: ((SwitchRequest) -> Void)?
    var sources: () -> [String] = { [] }   // the messages of the current source list (LinkProtocol.sourceMessages)
    var current: () -> String = { "" }     // the id of the input the monitor is on, "" when not known

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var controlChar: CBCharacteristic?
    private var updateChar: CBCharacteristic?
    private var upManifest: [Data] = []
    private var upImage = Data()
    private var upTarget = ""
    private var upSent = 0, upWritten = 0, upFrame = 180
    private var upWaitingWrite = false
    private var upTimer: Timer?
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

    // Sends the signed envelope and the app image (13.11). The caller has checked the key and the version.
    func startUpdate(envelope: Data, image: Data, version: String) {
        guard case .connected = state, let p = peripheral, updateChar != nil else { update = .failed("触摸屏没连上，或者它的固件还不支持蓝牙升级（要 0.3.22 以后）"); return }
        if case .sending = update { return }
        upFrame = max(23, min(p.maximumWriteValueLength(for: .withoutResponse), 509))
        upManifest = UpdateFrames.manifestFrames(envelope, maxFrame: upFrame)
        guard !upManifest.isEmpty else { update = .failed("清单太大"); return }
        upImage = image
        upTarget = version
        upSent = 0
        upWritten = 0
        update = .manifest
        log("start \(version) \(image.count) bytes, frame \(upFrame)")
        writeNextManifest()
    }
    func cancelUpdate() {
        guard let p = peripheral, let c = updateChar else { return }
        switch update {
        case .manifest, .waitingReady, .sending, .finishing:
            p.writeValue(UpdateFrames.abort, for: c, type: .withResponse)
            finishUpdate(.failed("已取消。旧固件照常运行。"))
        default: break
        }
    }
    private func log(_ s: String) { onUpdateLog?(s) }
    private func finishUpdate(_ end: UpdatePhase) {
        upTimer?.invalidate()
        upTimer = nil
        upManifest = []
        upImage = Data()
        upWaitingWrite = false
        update = end
        log("end \(end)")
    }
    private func arm(_ seconds: TimeInterval, _ why: String) {
        upTimer?.invalidate()
        upTimer = Timer.scheduledTimer(withTimeInterval: seconds, repeats: false) { [weak self] _ in self?.finishUpdate(.failed(why)) }
    }
    private func writeNextManifest() {
        guard let p = peripheral, let c = updateChar else { return }
        upWaitingWrite = true
        if !upManifest.isEmpty {
            p.writeValue(upManifest.removeFirst(), for: c, type: .withResponse)
        } else {
            update = .waitingReady
            arm(10, "设备 10 秒没有回答“准备好”。旧固件照常运行。")
            p.writeValue(UpdateFrames.begin, for: c, type: .withResponse)
        }
    }
    private func pump() {
        guard case .sending = update, let p = peripheral, let c = updateChar else { return }
        let per = UpdateFrames.dataPerFrame(maxFrame: upFrame)
        while upSent < upImage.count {
            let n = min(per, upImage.count - upSent)
            guard UpdateFrames.maysend(sent: upSent, written: upWritten, next: n), p.canSendWriteWithoutResponse else { return }
            let chunk = upImage.subdata(in: upSent..<(upSent + n))
            p.writeValue(UpdateFrames.dataFrame(offset: UInt32(upSent), chunk), for: c, type: .withoutResponse)
            upSent += n
        }
    }
    private func handle(_ e: UpdateEvent) {
        log("event \(e)")
        switch (e, update) {
        case (.ready, .waitingReady):
            update = .sending(0)
            arm(30, "设备 30 秒没有写入。旧固件照常运行。")
            pump()
        case (.writing(let n), .sending):
            upWritten = n
            update = .sending(Double(n) / Double(max(1, upImage.count)))
            arm(30, "设备 30 秒没有写入。旧固件照常运行。")
            if n >= upImage.count, let p = peripheral, let c = updateChar {
                update = .finishing
                arm(30, "设备 30 秒没有完成。")
                p.writeValue(UpdateFrames.end, for: c, type: .withResponse)
            } else { pump() }
        case (.rebooting, _):
            update = .restarting(upTarget)
            arm(90, "设备重启后 90 秒内没读回版本：不算确认。")
        case (.refused(let why), _), (.failed(let why), _):
            if update != .idle { finishUpdate(.failed(UpdateEvent.reason(why))) }
        default: break
        }
    }
    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) { pump() }

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
        updateChar = nil
        queue.removeAll()
        writing = false
        lastCurrent = nil
        switch update {   // U10: the device drops the update; the old firmware runs on
        case .manifest, .waitingReady, .sending, .finishing: finishUpdate(.failed("中途断开了。旧固件照常运行，可以再试一次。"))
        default: break
        }
        if remembered == p.identifier { connect(p) }   // reconnect by itself
        else if state != .searching { state = .notPaired }
    }
    // The touch screen said its table changed (every secure connection since 0.3.23): look again, so a characteristic a
    // newer firmware added (the update one) is found. The cached table hid it before (2026-11-07).
    func peripheral(_ p: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        controlChar = nil
        updateChar = nil
        p.discoverServices([Self.service])
    }
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == Self.service }) else { problem = "这个设备没有 DeskHub 的服务"; return }
        p.discoverCharacteristics([Self.info, Self.control, Self.events, Self.update], for: s)
    }
    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        guard let chars = s.characteristics else { return }
        controlChar = chars.first { $0.uuid == Self.control }
        updateChar = chars.first { $0.uuid == Self.update }
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
            info = ch.value.flatMap(DeviceInfo.parse)
            if case .restarting(let target) = update {   // K3: the version read back after the restart
                let got = info?.firmware ?? "?"
                finishUpdate(got == target ? .confirmed(got) : .failed("读回的版本是 \(got)，不是 \(target)：没有生效（自检不过会回到旧版本）。"))
            }
            state = .connected(p.name ?? "DeskHub")
            if let events = p.services?.first?.characteristics?.first(where: { $0.uuid == Self.events }) { p.setNotifyValue(true, for: events) }
            resendSources()
        } else if ch.uuid == Self.events, let v = ch.value {
            if let e = UpdateEvent.parse(v) { handle(e) }
            else if let r = LinkProtocol.parseRequest(v) { onRequest?(r) }
        }
    }
    func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if ch.uuid == Self.update {
            if let error { finishUpdate(.failed("发给触摸屏失败：\(error.localizedDescription)")); return }
            if upWaitingWrite, update == .manifest { writeNextManifest() }
            return
        }
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
