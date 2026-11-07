// The DDC/CI "VCP feature reply", checked byte by byte. Pure, so the rules are tested without a monitor.
//   [0] 0x6E source   [1] 0x88 length   [2] 0x02 opcode   [3] result: 0 ok, 1 unsupported
//   [4] VCP code      [5] type          [6..7] maximum    [8..9] current   [10] checksum = 0x50 ^ bytes 0...9
import Foundation

public enum DDCReplyError: Error, Equatable {
    case notReady          // the DDC/CI null message (6E 80 BE): the monitor had no reply yet; asking again is safe for a read
    case short(Int)
    case checksum(expected: UInt8, got: UInt8)
    case opcode(UInt8)
    case code(expected: UInt8, got: UInt8)
    case unsupported
    case result(UInt8)
}

public struct VCPFields: Equatable, Sendable {
    public var current: UInt16
    public var maximum: UInt16
}

public enum DDCReply {
    public static func parseVCPFields(_ b: [UInt8], vcp: UInt8) -> Result<VCPFields, DDCReplyError> {
        if b.count >= 3 && b[1] == 0x80 && b[2] == (b[0] ^ b[1] ^ 0x50) { return .failure(.notReady) }
        guard b.count >= 11 else { return .failure(.short(b.count)) }
        let sum = b[0..<10].reduce(UInt8(0x50)) { $0 ^ $1 }
        guard sum == b[10] else { return .failure(.checksum(expected: sum, got: b[10])) }
        guard b[2] == 0x02 else { return .failure(.opcode(b[2])) }
        if b[3] == 0x01 { return .failure(.unsupported) }
        guard b[3] == 0x00 else { return .failure(.result(b[3])) }
        guard b[4] == vcp else { return .failure(.code(expected: vcp, got: b[4])) }
        return .success(VCPFields(current: UInt16(b[8]) << 8 | UInt16(b[9]), maximum: UInt16(b[6]) << 8 | UInt16(b[7])))
    }
}
