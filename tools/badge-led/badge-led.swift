// badge-led: set a HAVEN badge's RGB LED from macOS over Bluetooth.
//
//   badge-led red | green | blue | white | on | off | #ff8800 | status
//   badge-led get                     print the current command
//   badge-led --name HAVEN-BADGE-2 red
//
// Uses the connection macOS already has to the badge as a keyboard; when the
// badge isn't connected, scans for it by name.

import CoreBluetooth
import Foundation

let ledService = CBUUID(string: "f1465b1e-5f0b-4bf3-9226-f072b6f56489")
let ledCommand = CBUUID(string: "83153295-8750-46cf-8630-d4bcd57865f6")
let hidService = CBUUID(string: "1812")

func fail(_ message: String) -> Never {
    FileHandle.standardError.write("badge-led: \(message)\n".data(using: .utf8)!)
    exit(1)
}

var args = Array(CommandLine.arguments.dropFirst())
var namePrefix = "HAVEN-BADGE"
if let i = args.firstIndex(of: "--name"), i + 1 < args.count {
    namePrefix = args[i + 1]
    args.removeSubrange(i...(i + 1))
}
guard args.count == 1 else {
    print("usage: badge-led [--name PREFIX] red|green|blue|white|yellow|cyan|magenta|orange|purple|pink|on|off|#rrggbb|status|get")
    exit(2)
}
let command = args[0]
let reading = command == "get"
if !reading && command.utf8.count > 16 {
    fail("command too long")
}

final class Client: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var candidates: [CBPeripheral] = []
    var current: CBPeripheral?
    var scanning = false

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            // Badges macOS is already connected to (as a keyboard). The LED service may be
            // missing from macOS's cache until it rediscovers, so also try every connected HID device.
            var seen = Set<UUID>()
            candidates = (c.retrieveConnectedPeripherals(withServices: [ledService])
                + c.retrieveConnectedPeripherals(withServices: [hidService]))
                .filter { seen.insert($0.identifier).inserted }
            tryNext()
        case .unauthorized:
            fail("Bluetooth permission denied. Allow your terminal app in System Settings > Privacy & Security > Bluetooth.")
        case .poweredOff:
            fail("Bluetooth is off")
        case .unsupported:
            fail("Bluetooth LE is not supported here")
        default:
            break
        }
    }

    func tryNext() {
        if candidates.isEmpty {
            if !scanning {
                scanning = true
                central.scanForPeripherals(withServices: nil)
            }
            return
        }
        let p = candidates.removeFirst()
        current = p
        p.delegate = self
        central.connect(p) // for a connected peripheral this only adds a reference to the existing link
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData: [String: Any], rssi: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? p.name ?? ""
        guard name.hasPrefix(namePrefix) else { return }
        c.stopScan()
        current = p
        p.delegate = self
        c.connect(p)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        p.discoverServices([ledService])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        tryNext()
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == ledService }) else {
            central.cancelPeripheralConnection(p) // not a badge, or firmware without the LED service
            tryNext()
            return
        }
        p.discoverCharacteristics([ledCommand], for: s)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        guard let ch = s.characteristics?.first(where: { $0.uuid == ledCommand }) else {
            fail("\(p.name ?? "badge"): LED characteristic missing")
        }
        if reading {
            p.readValue(for: ch)
        } else {
            p.writeValue(command.data(using: .utf8)!, for: ch, type: .withResponse)
        }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if let error = error {
            fail("write failed: \(error.localizedDescription)")
        }
        print("\(p.name ?? "badge"): \(command)")
        exit(0)
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        if let error = error {
            fail("read failed: \(error.localizedDescription)")
        }
        // Device OS pads the characteristic to its full length with NULs.
        let value = (ch.value ?? Data()).prefix { $0 != 0 }
        print(String(data: value, encoding: .utf8) ?? "")
        exit(0)
    }
}

let client = Client()
DispatchQueue.main.asyncAfter(deadline: .now() + 15) {
    fail("no \(namePrefix)* badge with the LED service found (is it paired, in range, and running haven-badge firmware?)")
}
RunLoop.main.run()
