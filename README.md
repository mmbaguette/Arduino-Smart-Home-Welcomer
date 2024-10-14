# Arduino Smart Home Greeter

### This device greets you when you come home with an LCD screen and emails the owner that you've arrived!

![image](https://user-images.githubusercontent.com/76597978/174444223-ce1790ad-2990-4e25-bdf9-99b5e912cdc1.png)

### Description
Normally, whenever someone comes back home, their smartphone should automatically connect to Wi-Fi. In this case, an ESP8266 Arduino microcontroller waits for any devices broadcasting a DHCP Discover packet, indicating that they’ve just connected to the network and arrived home. If this device is recognized on the network, it greets the person at the door using their phone’s name, obtained through the MDNS protocol.

## Features
 - Detects when your phone automatically joins your WiFi
 - Greets you with an LCD screen at your door with your name
 - Stores IP Addresses and MAC addresses in an array, which is saved on the device's EEPROM storage
 - Ability to add (A on keypad) devices through their IP Address on the network or to delete (D) them with their device number (# to submit)
 - When you add an IP Address to the list using your keypad, it pings the IP and stores the device's MAC address if it responds
 - Automatically picks up added devices' MDNS/Apple-Bonjour name (only on Apple) or a "Device X" name
 - List of added device names, IPs, and MACs is sent with every notification


![greeter2](https://user-images.githubusercontent.com/76597978/173166241-d3d8eba8-d2a7-4b16-9951-e1c9a6f12070.jpg)

## Requirements
- x1 Arduino Uno
- x1 ESP8266/NodeMCU
- x1 16x2 LCD Screen
- x1 4x4 Keypad
- x1 Potentiometer
- x23 male-to-male wires
- x1 Breadboard (optional)

## Wiring
- 8 wires for keypad, left to right, pins 11 to 4
- 1 wire for transmitting from Arduino Uno (pin 2) to ESP8266 (D1)
- LCD Screen to ESP8266 (reference: https://diyi0t.com/lcd-display-tutorial-for-arduino-and-esp8266/)
- 1 wire from Arduino Uno GND to negative row on breadboard, and 1 wire from Vin to positive row on breadboard
