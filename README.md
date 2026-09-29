# Arduino Smart Home Greeter

A two-microcontroller device that notices when a known phone joins the home Wi-Fi network, greets the person by name on an LCD at the door, and emails the owner with the name of the phone. It works entirely by listening to ordinary network traffic, with no app on the phone and no access to the router.

Built in June 2022 on an ESP8266 (NodeMCU) and an Arduino Uno, in C++ (~670 lines on the ESP8266).

![Greeter on the breadboard](https://user-images.githubusercontent.com/76597978/174444223-ce1790ad-2990-4e25-bdf9-99b5e912cdc1.png)

---

## How it works

```
 4x4 keypad ──► Arduino Uno ──(1-wire serial, 4800 baud)──► ESP8266 ──► 16x2 LCD
                                                               │
                              home Wi-Fi network  ◄────────────┤
                                ├─ DHCP broadcasts (UDP 67)  ──► arrival detection
                                ├─ ICMP ping + ARP            ──► device registration
                                ├─ mDNS answers               ──► device names
                                └─ SMTP (port 587)            ──► email alerts
```

The ESP8266 does all the networking. The Uno only scans the keypad and forwards each key press as a single ASCII character over a one-way serial link, which keeps the ESP8266's limited GPIO pins free for the LCD.

---

## Detecting an arrival: reading DHCP broadcasts

When a phone reconnects to Wi-Fi, it asks for an IP address using DHCP. Because it doesn't have an address yet, it **broadcasts** that request to `255.255.255.255` on UDP port 67, so every device on the network receives it, including this one.

The ESP8266 listens on that port and reads the client's MAC address (its hardware identifier) straight out of the DHCP header:

| Offset (bytes) | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | `op` | Request or reply |
| 1 | 1 | `htype` | Hardware type (Ethernet) |
| 2 | 1 | `hlen` | Hardware address length (6) |
...
| 12 | 16 | `ciaddr`, `yiaddr`, `siaddr`, `giaddr` | IP address fields (4 × 4 bytes) |
| **28** | **6** (of 16) | **`chaddr`** | **Client hardware (MAC) address** |

So the MAC always starts at byte 28. The code rejects packets shorter than 33 bytes, copies bytes 28–33, and looks the MAC up in the saved device list. On a match, the LCD shows a 45-second welcome with the device's name and an email alert is queued.

**Why match on MAC and not IP:** the router can hand a phone a different IP address each time it reconnects, but the MAC in the DHCP request identifies the device itself.

---

## Registering a device: ping, then read the MAC

Users add a device by typing its current IP address on the keypad (`A` to start, `*` for `.`, `B` to backspace, `#` to submit).

The device list needs MACs, but the user only knows the IP. So the ESP8266 **pings** that IP. Before the ping can be sent, the ESP8266's network stack has to resolve the IP to a MAC address using ARP (the protocol that maps IP addresses to hardware addresses on a local network). When the ping reply arrives, the MAC is available alongside it, and the device stores both.

Duplicate MACs or IPs are rejected, and the list is capped at 10 devices.

---

## Naming devices: passive mDNS

Apple devices announce their names on the local network using mDNS (Bonjour). The ESP8266 listens for mDNS address records, and when one matches a saved IP, it stores the hostname with the `.local` suffix stripped. Devices that never announce a name are shown as `Device 1`, `Device 2`, and so on.

---

## Persistent storage layout

The ESP8266 has no true EEPROM, so the `ESP_EEPROM` library emulates it in a sector of flash memory. The device list survives power loss using this fixed layout:

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 bytes | Number of saved devices (`int`) |
| 4 + 10·*i* | 6 bytes | MAC address of device *i* |
| 10 + 10·*i* | 4 bytes | IP address of device *i* |

Total: 4 + 10 × 10 = 104 bytes. The list is loaded on boot and rewritten and committed after every add or delete.

---

## Keeping the loop responsive

Nothing in the main loop blocks for long:

- UDP packets, ping replies and email results arrive through **callbacks** (functions the library calls when an event happens), so the loop never waits on the network.
- LCD messages are **timed with `millis()`** rather than `delay()`: each message has a duration and a fallback text, and the display reverts on its own when the time is up.
- Email alerts are **rate-limited** to one every 5 minutes 6 seconds, so a phone that reconnects repeatedly doesn't flood the inbox.

---

## Hardware

- 1 × Arduino Uno (keypad scanner)
- 1 × ESP8266 / NodeMCU
- 1 × 16×2 character LCD, with a potentiometer for contrast
- 1 × 4×4 membrane keypad
- Breadboard and 23 jumper wires

### Wiring

- Keypad → Uno pins 11 to 4 (8 wires, left to right)
- Uno pin 2 (TX) → ESP8266 D1 (RX), one-way serial at 4800 baud
- LCD → ESP8266 (RS 4, EN 0, D4–D7 on 12, 13, 15, 3); see [this LCD wiring reference](https://diyi0t.com/lcd-display-tutorial-for-arduino-and-esp8266/)
- Uno GND and Vin → breadboard power rails
