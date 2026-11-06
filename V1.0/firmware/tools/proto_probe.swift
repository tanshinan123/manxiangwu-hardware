// Protocol-level probe for the SlowHouse-Brain companion.
// Exercises the JSON frames from plan section 5 in both directions:
//   phone -> device : STATE_SYNC, FOCUS_START, duplicate eventID
//   device -> phone : STATE_REQUEST (on subscribe), STATE_ACK, TIME_UP
//
// Usage: swift proto_probe.swift

import Foundation
import CoreBluetooth

let SERVICE = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345601")
let WRITE   = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345602")
let ACK     = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345603")

func stamp() -> String {
    let f = DateFormatter()
    f.dateFormat = "HH:mm:ss.SSS"
    return f.string(from: Date())
}

func frame(_ type: String, eventID: String, session: String = "", score: Int? = nil,
           seconds: Int? = nil, breakType: String? = nil) -> Data {
    var payload: [String] = []
    if let score { payload.append("\"brainScore\":\(score)") }
    if let seconds { payload.append("\"durationSeconds\":\(seconds)") }
    if let breakType { payload.append("\"breakType\":\"\(breakType)\"") }
    let json = """
    {"protocolVersion":1,"eventID":"\(eventID)","sessionID":"\(session)",\
    "eventType":"\(type)","timestamp":\(Int(Date().timeIntervalSince1970)),\
    "payload":{\(payload.joined(separator: ","))}}
    """
    return Data(json.utf8)
}

final class Probe: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var target: CBPeripheral?
    var writeChr: CBCharacteristic?
    var ackChr: CBCharacteristic?
    var done = false
    var seen: [String] = []
    /// With --extend the probe answers the first TIME_UP with FOCUS_EXTEND, so
    /// the second (post-extension) decision page gets exercised too.
    let extendMode = CommandLine.arguments.contains("--extend")
    var extendedSent = false
    let started = Date()

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func finish(_ code: Int32, _ note: String) {
        guard !done else { return }
        done = true
        print("\n==== \(note) ==== events=\(seen.count) \(String(format: "%.1fs", Date().timeIntervalSince(started)))")
        exit(code)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        guard c.state == .poweredOn else { return }
        print("[\(stamp())] scanning")
        c.scanForPeripherals(withServices: [SERVICE], options: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in
            if self?.target == nil { self?.finish(2, "device not found") }
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard target == nil else { return }
        target = p
        central.stopScan()
        p.delegate = self
        central.connect(p, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect p: CBPeripheral) {
        print("[\(stamp())] connected, discovering")
        p.discoverServices([SERVICE])
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        if !done { print("[\(stamp())] disconnected") }
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == SERVICE }) else {
            finish(3, "service missing"); return
        }
        p.discoverCharacteristics([WRITE, ACK], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for chr in service.characteristics ?? [] {
            if chr.uuid == WRITE { writeChr = chr }
            if chr.uuid == ACK { ackChr = chr }
        }
        guard let ack = ackChr, writeChr != nil else { finish(4, "chars missing"); return }
        p.setNotifyValue(true, for: ack)
        // STATE_REQUEST should arrive right after the subscription is registered.
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { self.step1() }
    }

    func send(_ name: String, _ data: Data) {
        guard let p = target, let wr = writeChr else { return }
        print("[\(stamp())] -> \(name) (\(data.count)B)")
        p.writeValue(data, for: wr, type: .withResponse)
    }

    func step1() {
        send("STATE_SYNC score=40", frame("STATE_SYNC", eventID: "app-sync-1", score: 40))
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { self.step2() }
    }

    func step2() {
        send("FOCUS_START 6s", frame("FOCUS_START", eventID: "app-focus-1",
                                     session: "sess-A", score: 40, seconds: 6))
        // Expect TIME_UP six seconds later (plus a second one when extending).
        DispatchQueue.main.asyncAfter(deadline: .now() + (extendMode ? 16.0 : 10.0)) { self.step3() }
    }

    func step3() {
        // Same eventID as step1: the device must drop it and still ACK.
        send("STATE_SYNC duplicate", frame("STATE_SYNC", eventID: "app-sync-1", score: 80))
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { self.step4() }
    }

    func step4() {
        send("BREAK_MENU", frame("BREAK_MENU", eventID: "app-menu-1", session: "sess-A"))
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
            self.central.cancelPeripheralConnection(self.target!)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { self.report() }
    }

    func report() {
        let hasRequest = seen.contains { $0.contains("STATE_REQUEST") }
        let hasAck = seen.contains { $0.contains("STATE_ACK") }
        let hasTimeUp = seen.contains { $0.contains("TIME_UP") }
        let timeUps = seen.filter { $0.contains("TIME_UP") }.count
        print("""

        ---- summary ----
        STATE_REQUEST (device asks for truth): \(hasRequest ? "yes" : "NO")
        STATE_ACK     (per write):             \(hasAck ? "yes" : "NO")
        TIME_UP       (countdown finished):    \(timeUps)
        """)
        finish(hasRequest && hasAck && hasTimeUp && timeUps >= (extendMode ? 2 : 1) ? 0 : 1, "protocol probe")
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor chr: CBCharacteristic, error: Error?) {
        guard chr.uuid == ACK, let data = chr.value else { return }
        let text = String(data: data, encoding: .utf8) ?? data.map { String(format: "%02X", $0) }.joined(separator: " ")
        seen.append(text)
        print("[\(stamp())] <- \(text)")

        // First time-up: answer like the App would after the user taps 续时.
        if extendMode, !extendedSent, text.contains("TIME_UP") {
            extendedSent = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                self.send("FOCUS_EXTEND 5s", frame("FOCUS_EXTEND", eventID: "app-extend-1",
                                                   session: "sess-A", score: 40, seconds: 5))
            }
        }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor chr: CBCharacteristic, error: Error?) {
        if let error { print("[\(stamp())] write error: \(error.localizedDescription)") }
    }
}

let probe = Probe()
DispatchQueue.main.asyncAfter(deadline: .now() + 60) { probe.finish(9, "global timeout") }
RunLoop.main.run()
