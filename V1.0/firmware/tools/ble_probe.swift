// Mac-side BLE regression probe for the SlowHouse-Brain companion.
// Usage: swift ble_probe.swift [score...]
// Default sequence: 0x14 0x28 0x3C 0x50 0x64 0x00 0x15 (last one is invalid).
//
// Talks to the same GATT layout the iPhone nRF Connect flow uses:
//   12345678-1234-5678-9ABC-DEF012345601  service
//   12345678-1234-5678-9ABC-DEF012345602  write   (1 byte score)
//   12345678-1234-5678-9ABC-DEF012345603  notify  (status, score)

import Foundation
import CoreBluetooth

let SERVICE = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345601")
let WRITE   = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345602")
let ACK     = CBUUID(string: "12345678-1234-5678-9ABC-DEF012345603")

struct Case {
    let value: UInt8
    let expectStatus: UInt8
    let label: String
}

var cases: [Case] = [
    Case(value: 0x14, expectStatus: 0x00, label: "20"),
    Case(value: 0x28, expectStatus: 0x00, label: "40"),
    Case(value: 0x3C, expectStatus: 0x00, label: "60"),
    Case(value: 0x50, expectStatus: 0x00, label: "80"),
    Case(value: 0x64, expectStatus: 0x00, label: "100"),
    Case(value: 0x00, expectStatus: 0x00, label: "0"),
    Case(value: 0x15, expectStatus: 0x02, label: "15 (invalid)"),
]

let argv = CommandLine.arguments.dropFirst()
if !argv.isEmpty {
    cases = argv.compactMap { UInt8($0, radix: 16) }.map {
        Case(value: $0, expectStatus: ($0 % 20 == 0 && $0 <= 100) ? 0x00 : 0x02, label: "\($0)")
    }
}

func hex(_ data: Data) -> String {
    data.map { String(format: "%02X", $0) }.joined(separator: " ")
}

final class Probe: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var target: CBPeripheral?
    var writeChr: CBCharacteristic?
    var ackChr: CBCharacteristic?
    var index = 0
    var acks = 0
    var failures = 0
    var advertisingSeen = 0
    var ackTimeout: DispatchWorkItem?
    var finished = false
    let started = Date()

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func stop(_ code: Int32, _ message: String) {
        guard !finished else { return }
        finished = true
        print("\n==== \(message) ====")
        print(String(format: "elapsed %.1fs, acks=%d, failures=%d, adv_frames=%d",
                     Date().timeIntervalSince(started), acks, failures, advertisingSeen))
        exit(code)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        print("central state = \(c.state.rawValue) (5 = poweredOn)")
        guard c.state == .poweredOn else {
            if c.state == .unauthorized { stop(3, "Bluetooth permission denied") }
            return
        }
        print("scanning for SlowHouse-Brain ...")
        c.scanForPeripherals(withServices: nil, options: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 25) { [weak self] in
            guard let self, self.target == nil else { return }
            self.stop(2, "SlowHouse-Brain not found in 25s")
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
        guard name.contains("SlowHouse-Brain") else { return }
        advertisingSeen += 1
        print("found \(name) rssi=\(RSSI) id=\(p.identifier.uuidString)")
        guard target == nil else { return }
        target = p
        central.stopScan()
        p.delegate = self
        central.connect(p, options: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in
            guard let self, self.writeChr == nil else { return }
            self.stop(4, "connect or discovery timed out")
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect p: CBPeripheral) {
        print("connected, discovering service ...")
        p.discoverServices([SERVICE])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        stop(5, "connect failed: \(error?.localizedDescription ?? "unknown")")
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        if !finished && index < cases.count {
            stop(6, "unexpected disconnect at case \(index)")
        }
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == SERVICE }) else {
            stop(7, "service \(SERVICE.uuidString) missing")
            return
        }
        p.discoverCharacteristics([WRITE, ACK], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for chr in service.characteristics ?? [] {
            if chr.uuid == WRITE { writeChr = chr }
            if chr.uuid == ACK { ackChr = chr }
        }
        guard let ack = ackChr, let wr = writeChr else {
            stop(8, "characteristics missing")
            return
        }
        print("write props=\(wr.properties.rawValue) notify props=\(ack.properties.rawValue)")
        p.setNotifyValue(true, for: ack)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { [weak self] in
            self?.writeNext()
        }
    }

    func writeNext() {
        guard let p = target, let wr = writeChr else { return }
        if index >= cases.count {
            p.setNotifyValue(false, for: ackChr!)
            central.cancelPeripheralConnection(p)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
                self?.stop(0, "BLE regression PASSED")
            }
            return
        }
        let c = cases[index]
        print(String(format: "write 0x%02X (expect %@)", c.value, c.label))
        p.writeValue(Data([c.value]), for: wr, type: .withResponse)

        let timeout = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.failures += 1
            print("  !! no ACK for 0x\(String(format: "%02X", c.value))")
            self.index += 1
            self.writeNext()
        }
        ackTimeout = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0, execute: timeout)
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor chr: CBCharacteristic, error: Error?) {
        if let error {
            print("  write error (expected for invalid values): \(error.localizedDescription)")
        }
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor chr: CBCharacteristic, error: Error?) {
        guard chr.uuid == ACK, let data = chr.value, !data.isEmpty else { return }
        ackTimeout?.cancel()
        acks += 1
        let c = index < cases.count ? cases[index] : cases.last!
        let status = data.count > 0 ? data[0] : 0xFF
        let score = data.count > 1 ? data[1] : 0xFF
        let ok = status == c.expectStatus
        if !ok { failures += 1 }
        print("  ack [\(hex(data))] status=\(status) score=\(score) \(ok ? "OK" : "MISMATCH")")
        index += 1
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            self?.writeNext()
        }
    }
}

let probe = Probe()
DispatchQueue.main.asyncAfter(deadline: .now() + 90) {
    probe.stop(9, "global timeout")
}
RunLoop.main.run()
