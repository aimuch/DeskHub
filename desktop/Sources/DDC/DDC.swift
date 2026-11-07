// DDC/CI over IOAVService, the private I2C path of Apple silicon Macs (the approach of AppleSiliconDDC and m1ddc, both MIT,
// by waydabber; written here from the protocol, not copied). The only code in the App that talks to a monitor.
//
// Limits (docs/desktop-app-design.md 5.3): Apple silicon only; private API, so a macOS update may break it; the built-in
// screen has no DDC; many monitors report the current input badly, so a read-back is evidence only when it matches.
import ColorSync
import CoreGraphics
import DeskHubCore
import Foundation
import IOKit

public enum DDCError: Error, Equatable, CustomStringConvertible {
    case unavailable(String)    // the private functions or the service are not there
    case writeFailed(Int32)     // IOReturn of the write
    case readFailed(Int32)      // IOReturn of the read
    case badReply(String)       // a reply that does not check out (checksum, opcode, code)
    case unsupported            // the monitor says it does not have this VCP code

    public var description: String {
        switch self {
        case .unavailable(let why): return "DDC not available: \(why)"
        case .writeFailed(let r): return "DDC write failed (IOReturn \(r))"
        case .readFailed(let r): return "DDC read failed (IOReturn \(r))"
        case .badReply(let why): return "DDC reply not valid: \(why)"
        case .unsupported: return "the monitor does not support this VCP code"
        }
    }
}

public struct VCPValue: Equatable, Sendable {
    public var current: UInt16
    public var maximum: UInt16
}

// ---- the private functions, looked up at run time so that a missing one is an error, not a crash at launch ----
private typealias AVCreate = @convention(c) (CFAllocator?, io_service_t) -> Unmanaged<CFTypeRef>?
private typealias AVI2C = @convention(c) (CFTypeRef, UInt32, UInt32, UnsafeMutableRawPointer, UInt32) -> IOReturn
private typealias InfoDict = @convention(c) (CGDirectDisplayID) -> Unmanaged<CFDictionary>?

private struct PrivateAPI {
    let create: AVCreate
    let write: AVI2C
    let read: AVI2C
    let info: InfoDict

    static let shared: PrivateAPI? = {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
              let c = dlsym(iokit, "IOAVServiceCreateWithService"),
              let w = dlsym(iokit, "IOAVServiceWriteI2C"),
              let r = dlsym(iokit, "IOAVServiceReadI2C") else { return nil }
        let coreDisplay = dlopen("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay", RTLD_NOW)
            ?? dlopen("/System/Library/PrivateFrameworks/CoreDisplay.framework/CoreDisplay", RTLD_NOW)
        guard let cd = coreDisplay, let i = dlsym(cd, "CoreDisplay_DisplayCreateInfoDictionary") else { return nil }
        return PrivateAPI(create: unsafeBitCast(c, to: AVCreate.self), write: unsafeBitCast(w, to: AVI2C.self),
                          read: unsafeBitCast(r, to: AVI2C.self), info: unsafeBitCast(i, to: InfoDict.self))
    }()
}

// The DDC/CI addresses: the monitor is 0x37 on the bus (0x6E written); the host is 0x51 (0x50 in the reply checksum).
private let chip: UInt32 = 0x37
private let hostAddress: UInt32 = 0x51

public final class DDCDisplay: @unchecked Sendable {
    public let identity: DisplayIdentity
    public let displayID: CGDirectDisplayID
    private let service: CFTypeRef?
    private let lock = NSLock()   // one transaction at a time on a bus

    init(identity: DisplayIdentity, displayID: CGDirectDisplayID, service: CFTypeRef?) {
        self.identity = identity
        self.displayID = displayID
        self.service = service
    }

    public var hasDDC: Bool { service != nil }

    // Every external display that is online now, with its identity; `hasDDC` false when no AV service was found for it.
    public static func list() throws -> [DDCDisplay] {
        guard let api = PrivateAPI.shared else { throw DDCError.unavailable("IOAVService or CoreDisplay functions not found") }
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(16, &ids, &count) == .success else { throw DDCError.unavailable("no display list") }
        var out: [DDCDisplay] = []
        for id in ids.prefix(Int(count)) where CGDisplayIsBuiltin(id) == 0 {
            guard let cfuuid = CGDisplayCreateUUIDFromDisplayID(id)?.takeRetainedValue(),
                  let uuid = CFUUIDCreateString(nil, cfuuid) as String? else { continue }
            let info = api.info(id)?.takeRetainedValue() as? [String: Any] ?? [:]
            let location = info["IODisplayLocation"] as? String ?? ""
            let serial = (info["DisplaySerialNumber"] as? NSNumber).map { "\($0.uint32Value)" } ?? ""
            var name = ""
            if let names = info["DisplayProductName"] as? [String: String] { name = names["en_US"] ?? names.values.sorted().first ?? "" }
            let identity = DisplayIdentity(uuid: uuid.uppercased(), serial: serial == "0" ? "" : serial, location: location, name: name)
            out.append(DDCDisplay(identity: identity, displayID: id, service: location.isEmpty ? nil : avService(after: location, api: api)))
        }
        return out
    }

    // The AV service of a framebuffer: the first external DCPAVServiceProxy that follows it in the registry.
    private static func avService(after location: String, api: PrivateAPI) -> CFTypeRef? {
        var iterator: io_iterator_t = 0
        guard IORegistryCreateIterator(kIOMainPortDefault, kIOServicePlane, IOOptionBits(kIORegistryIterateRecursively), &iterator) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(iterator) }
        var found = false
        while case let entry = IOIteratorNext(iterator), entry != 0 {
            defer { IOObjectRelease(entry) }
            if !found {
                var path = [CChar](repeating: 0, count: 1024)
                if IORegistryEntryGetPath(entry, kIOServicePlane, &path) == KERN_SUCCESS, String(cString: path) == location { found = true }
                continue
            }
            var cls = [CChar](repeating: 0, count: 128)
            IOObjectGetClass(entry, &cls)
            guard String(cString: cls) == "DCPAVServiceProxy" else { continue }
            let where_ = IORegistryEntryCreateCFProperty(entry, "Location" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? String
            guard where_ == "External" else { continue }
            return api.create(kCFAllocatorDefault, entry)?.takeRetainedValue()
        }
        return nil
    }

    // The I2C path drops packets: like m1ddc and AppleSiliconDDC, each packet goes out twice, 10 ms apart. For a request
    // that is harmless; for a "set input" it sets the same value twice. Neither is a retry after a failure (K2).
    private func send(_ api: PrivateAPI, _ service: CFTypeRef, _ packet: [UInt8]) -> IOReturn {
        var result = kIOReturnSuccess
        for _ in 0..<2 {
            usleep(10_000)
            var copy = packet
            result = copy.withUnsafeMutableBytes { api.write(service, chip, hostAddress, $0.baseAddress!, UInt32($0.count)) }
        }
        return result
    }

    // Get VCP feature. A read changes nothing on the monitor, so a "not ready" or garbled reply may be asked for again
    // (3 times at most). A set is never repeated after a failure (K2).
    public func read(vcp: UInt8) throws -> VCPValue {
        guard let api = PrivateAPI.shared, let service else { throw DDCError.unavailable("no AV service for this display") }
        lock.lock(); defer { lock.unlock() }
        var last: DDCError = .badReply("no attempt")
        for attempt in 0..<3 {
            if attempt > 0 { usleep(50_000) }
            var request: [UInt8] = [0x82, 0x01, vcp, 0]
            request[3] = 0x6E ^ UInt8(hostAddress) ^ request[0] ^ request[1] ^ request[2]
            let w = send(api, service, request)
            guard w == kIOReturnSuccess else { last = .writeFailed(w); continue }
            usleep(UInt32(ProcessInfo.processInfo.environment["DESKHUB_DDC_WAIT_MS"].flatMap { UInt32($0) } ?? 10) * 1000)
            var reply = [UInt8](repeating: 0, count: 11)
            let r = reply.withUnsafeMutableBytes { api.read(service, chip, hostAddress, $0.baseAddress!, UInt32($0.count)) }
            guard r == kIOReturnSuccess else { last = .readFailed(r); continue }
            switch DDCReply.parseVCPFields(reply, vcp: vcp) {
            case .success(let v): return VCPValue(current: v.current, maximum: v.maximum)
            case .failure(.unsupported): throw DDCError.unsupported
            case .failure(let e): last = .badReply("\(e) raw \(reply.map { String(format: "%02x", $0) }.joined(separator: " "))")
            }
        }
        throw last
    }

    // Set VCP feature: one write. Whether it took effect is for a read-back to say (K3).
    public func write(vcp: UInt8, value: UInt16) throws {
        guard let api = PrivateAPI.shared, let service else { throw DDCError.unavailable("no AV service for this display") }
        lock.lock(); defer { lock.unlock() }
        var packet: [UInt8] = [0x84, 0x03, vcp, UInt8(value >> 8), UInt8(value & 0xFF), 0]
        packet[5] = packet[0..<5].reduce(0x6E ^ UInt8(hostAddress)) { $0 ^ $1 }
        let w = send(api, service, packet)
        guard w == kIOReturnSuccess else { throw DDCError.writeFailed(w) }
    }
}
