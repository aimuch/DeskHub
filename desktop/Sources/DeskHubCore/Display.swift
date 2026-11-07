// A monitor as the App knows it, and how a binding recognises it (rule K2: system UUID, serial number and IO location;
// all three must match before a command goes out).
import Foundation

public struct DisplayIdentity: Codable, Equatable, Hashable, Sendable {
    public var uuid: String       // the system UUID (CGDisplayCreateUUIDFromDisplayID), upper case
    public var serial: String     // the serial number the monitor reports; "" when it reports none
    public var location: String   // the IORegistry path of its framebuffer ("IOService:/AppleARMPE/...")
    public var name: String       // what the monitor calls itself: shown, never used to recognise it

    public init(uuid: String, serial: String, location: String, name: String) {
        self.uuid = uuid
        self.serial = serial
        self.location = location
        self.name = name
    }
}
