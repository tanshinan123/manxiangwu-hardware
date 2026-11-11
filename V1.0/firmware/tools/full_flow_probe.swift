// End-to-end probe for both product flows.
//
// Flow 1: six brain levels -> state sync -> focus -> time-up -> extend ->
//         time-up -> break menu/action -> focus end
// Flow 2: eight-skin mask incl. health -> wearing sync -> four postcards ->
//         two insight cards -> capability ACK
// Then disconnect and reconnect once to verify re-advertising.
//
// Usage: swift tools/full_flow_probe.swift

import Foundation
import CoreBluetooth

let SERVICE = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345601")
let WRITE = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345602")
let ACK = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345603")

func stamp() -> String {
    let f = DateFormatter()
    f.dateFormat = "HH:mm:ss.SSS"
    return f.string(from: Date())
}

func frame(_ type: String, event: String, session: String = "",
           payload: [String: Any] = [:]) -> Data {
    let body: [String: Any] = [
        "protocolVersion": 1,
        "eventID": event,
        "sessionID": session,
        "eventType": type,
        "timestamp": Int(Date().timeIntervalSince1970),
        "payload": payload,
    ]
    return try! JSONSerialization.data(withJSONObject: body)
}

final class FullFlowProbe: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var target: CBPeripheral?
    var writeChr: CBCharacteristic?
    var ackChr: CBCharacteristic?
    var finished = false
    var started = Date()
    var scoreIndex = 0
    let restoreOnly = CommandLine.arguments.contains("--restore-only")
    let scores: [UInt8] = [0, 20, 40, 60, 80, 100]
    var scoreAcks = 0
    var timeUps = 0
    var extended = false
    var sawStateRequest = 0
    var sawCapabilities = false
    var reconnectExpected = false
    var reconnected = false
    var writes = 0

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func log(_ text: String) { print("[\(stamp())] \(text)") }

    func finish(_ code: Int32, _ note: String) {
        guard !finished else { return }
        finished = true
        print("""

        ==== \(note) ====
        elapsed: \(String(format: "%.1f", Date().timeIntervalSince(started)))s
        score ACKs: \(scoreAcks)/6
        STATE_REQUEST: \(sawStateRequest)
        capabilities: \(sawCapabilities ? "yes" : "NO")
        TIME_UP: \(timeUps)
        writes: \(writes)
        reconnected: \(reconnected)
        """)
        exit(code)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        guard c.state == .poweredOn else {
            if c.state == .unauthorized { finish(3, "Bluetooth permission denied") }
            return
        }
        log("scanning")
        c.scanForPeripherals(withServices: [SERVICE], options: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in
            guard let self, self.target == nil else { return }
            self.finish(2, "device not found")
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        guard target == nil else { return }
        target = p
        c.stopScan()
        p.delegate = self
        if scoreAcks == 6 { reconnected = true }
        log("found rssi=\(RSSI.intValue), connecting")
        c.connect(p, options: nil)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("connected")
        p.discoverServices([SERVICE])
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral,
                        error: Error?) {
        log("disconnected: \(error?.localizedDescription ?? "clean")")
        writeChr = nil
        ackChr = nil
        guard !finished else { return }
        if reconnectExpected {
            reconnectExpected = false
            target = nil
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                self.log("scanning again after disconnect")
                self.central.scanForPeripherals(withServices: [SERVICE], options: nil)
            }
        } else {
            finish(6, "unexpected disconnect")
        }
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        finish(4, "connect failed: \(error?.localizedDescription ?? "-")")
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == SERVICE }) else {
            finish(7, "service missing")
            return
        }
        p.discoverCharacteristics([WRITE, ACK], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService,
                    error: Error?) {
        for chr in svc.characteristics ?? [] {
            if chr.uuid == WRITE { writeChr = chr }
            if chr.uuid == ACK { ackChr = chr }
        }
        guard let ack = ackChr, writeChr != nil else {
            finish(8, "characteristics missing")
            return
        }
        p.setNotifyValue(true, for: ack)
        if restoreOnly {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
                self.restoreNoSkin()
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                    self.finish(0, "RESTORE NO SKIN")
                }
            }
            return
        }
        if reconnected {
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                self.finish(self.scoreAcks == 6 && self.timeUps >= 2 && self.sawStateRequest >= 2 &&
                            self.sawCapabilities && self.reconnected ? 0 : 1,
                            "BOTH FLOW PROBE")
            }
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { self.sendScore() }
        }
    }

    func write(_ name: String, _ data: Data) {
        guard let p = target, let chr = writeChr else {
            log("cannot write \(name): link missing")
            return
        }
        writes += 1
        log("-> \(name) (\(data.count)B)")
        p.writeValue(data, for: chr, type: .withResponse)
    }

    func sendScore() {
        guard scoreIndex < scores.count else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { self.sendFlowTwo() }
            return
        }
        let value = scores[scoreIndex]
        write(String(format: "score %u", value), Data([value]))
    }

    func sendFlowTwo() {
        let sync = frame("STATE_SYNC", event: "sync-health", payload: [
            "brainScore": 60,
            "unlockedSkinMask": 0xFF,
            "wearingSkin": "health",
        ])
        write("flow2 STATE_SYNC mask=0xFF health", sync)
        let postcards: [(Int, Bool, String, String)] = [
            (0, true, "掉电中", "i人持续掉电中，我的充电宝呢"),
            (1, false, "", ""),
            (2, false, "", ""),
            (3, true, "信息流", "随时冲散在信息流中无法自拔"),
        ]
        // Deliberate burst: this is the pattern that used to kill the ACL pool.
        for card in postcards {
            let data = frame("CONTENT_SYNC", event: "content-postcard-\(card.0)", payload: [
                "contentKind": "postcard", "index": card.0, "present": card.1,
                "title": card.2, "text": card.3,
            ])
            write("postcard \(card.0)", data)
        }
        let insights = ["按时结束的比例提高了25%，继续加油哦～", "已读不回"]
        for (index, title) in insights.enumerated() {
            let data = frame("CONTENT_SYNC", event: "content-insight-\(index)", payload: [
                "contentKind": "insight", "index": index, "present": true, "title": title,
            ])
            write("insight \(index)", data)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) { self.sendFocus() }
    }

    func sendFocus() {
        write("FLOW1 FOCUS_START 6s", frame("FOCUS_START", event: "focus-1", session: "probe-session",
            payload: ["durationSeconds": 6, "brainScore": 60]))
        DispatchQueue.main.asyncAfter(deadline: .now() + 15.0) { [weak self] in
            guard let self, self.timeUps < 2 else { return }
            self.finish(5, "TIME_UP missing")
        }
    }

    func restoreNoSkin() {
        // All skins remain unlocked; clearing wearing restores the six score
        // images on the realtime page.
        write("restore STATE_SYNC no skin", frame("STATE_SYNC", event: "sync-restore",
            payload: ["brainScore": 60, "unlockedSkinMask": 0xFF, "wearingSkin": ""]))
    }

    func sendBreakAndEnd() {
        write("FLOW1 BREAK_MENU", frame("BREAK_MENU", event: "break-menu-1",
            session: "probe-session", payload: ["items": ["water", "move", "sleep", "return"],
                                                 "expiresInSeconds": 15]))
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
            self.write("FLOW1 BREAK_ACTION water", frame("BREAK_ACTION", event: "break-action-1",
                session: "probe-session", payload: ["breakType": "water"]))
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 5.0) {
            self.write("FLOW1 FOCUS_END", frame("FOCUS_END", event: "focus-end-1",
                session: "probe-session", payload: ["brainScore": 80]))
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 6.5) {
            self.restoreNoSkin()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 8.0) {
            self.reconnectExpected = true
            if let target = self.target {
                self.central.cancelPeripheralConnection(target)
            }
        }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor chr: CBCharacteristic, error: Error?) {
        if let error { log("write error: \(error.localizedDescription)") }
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor chr: CBCharacteristic, error: Error?) {
        guard chr.uuid == ACK, let data = chr.value else { return }
        if let type = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let eventType = type["eventType"] as? String {
            let payload = type["payload"] as? [String: Any] ?? [:]
            log("<- \(eventType) \(data.count)B")
            if eventType == "STATE_REQUEST" {
                sawStateRequest += 1
            }
            if eventType == "STATE_ACK" {
                if let caps = payload["capabilities"] as? [String], caps.contains("CONTENT_SYNC") {
                    sawCapabilities = true
                }
            }
            if eventType == "TIME_UP" {
                timeUps += 1
                if timeUps == 1, !extended {
                    extended = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                        self.write("FLOW1 FOCUS_EXTEND 4s", frame("FOCUS_EXTEND",
                            event: "extend-1", session: "probe-session",
                            payload: ["durationSeconds": 4, "brainScore": 60]))
                    }
                } else if timeUps == 2 {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                        self.sendBreakAndEnd()
                    }
                }
            }
        } else if data.count == 2 {
            let status = data[0], score = data[1]
            let expected = scoreIndex < scores.count ? scores[scoreIndex] : 0xFF
            let ok = status == 0 && score == expected
            if ok { scoreAcks += 1 }
            log("<- one-byte ACK status=\(status) score=\(score) \(ok ? "OK" : "MISMATCH")")
            scoreIndex += 1
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { self.sendScore() }
        }
    }

}

let probe = FullFlowProbe()
// SECOND_DISCOVERY marker: the first reconnection after the intentional drop.
DispatchQueue.main.asyncAfter(deadline: .now() + 60) { probe.finish(9, "global timeout") }
RunLoop.main.run()
