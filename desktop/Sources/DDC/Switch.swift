// One switch of the input source, the same for the menu, the trial, the command line and (later) the touch screen:
// one write of VCP 0x60, then read-backs after readBackDelays, judged by classify() (K3). Never repeated (K2).
import DeskHubCore
import Foundation

public let inputSourceVCP: UInt8 = 0x60

public func readInput(_ display: DDCDisplay) -> Result<UInt16, ReadError> {
    do { return .success(try display.read(vcp: inputSourceVCP).current) } catch { return .failure(ReadError("\(error)")) }
}

// Blocks for about 4 s: call it off the main thread.
public func performSwitch(_ display: DDCDisplay, to target: UInt16, sleep: (Double) -> Void = { Thread.sleep(forTimeInterval: $0) }) -> SwitchResult {
    // Already there (read back, K3): no command. A monitor told to switch to the input it shows blanks and syncs again.
    if case .success(let now) = readInput(display), inputMatches(readBack: now, target: target) {
        return SwitchResult(outcome: .confirmed, detail: "", readBack: now)
    }
    do {
        try display.write(vcp: inputSourceVCP, value: target)
    } catch {
        return classify(sendError: "DDC 写失败（\(error)）", readBacks: [], target: target)
    }
    var reads: [Result<UInt16, ReadError>] = []
    var waited = 0.0
    for at in readBackDelays {
        sleep(at - waited)
        waited = at
        let r = readInput(display)
        reads.append(r)
        if case .success(let v) = r, inputMatches(readBack: v, target: target) { break }   // confirmed: no need to wait more
    }
    return classify(sendError: nil, readBacks: reads, target: target)
}
