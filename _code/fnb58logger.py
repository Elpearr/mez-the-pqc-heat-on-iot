# fnb58logger.py
# Logs voltage and current from the FNIRSI FNB58 over USB HID (Windows, hidapi).
#
# Protocol (start commands, keep-alive command, packet layout) is based on:
#   Witold Baryluk (baryluk) (2024) fnirsi-usb-power-data-logger, commit 746e4d3, MIT licence.
#
# Written with Claude: the protocol parts (command bytes, sendCommand, startStream,
#   decodePacket, drainMeter) and code suggestions for the per-sample rows, flush/fsync,
#   silent-meter and disconnect handling, which I (Callum) adapted and debugged.
# Written by me (Callum): CSV setup, counters, summary, and testing against real data.

import hid
import sys
import time
import os
import csv

# declare FNB58's vendor and product IDs
vendorId = 0x2E3C # 11836
productId = 0x5558 # 21848

sampleInterval = 0.01   # the meter sends 100 samples a second: 4 per packet, one packet every 40 ms

# seconds between keep-alive commands. changed from 1.0 to 0.1 on 8 Nov 2026 after the meter
# froze with 1.0; baryluk's repo reports similar freezes, cause unknown. not yet proven to fix it.
keepAliveEvery = 0.1    
 
# Each command is 64 bytes: 0xaa, a command byte, 61 zero bytes, then a check byte (CRC-8).
# The check bytes are fixed because the commands never change.
startCmd1 = bytes([0xaa, 0x81]) + bytes(61) + bytes([0x8e])
startCmd2 = bytes([0xaa, 0x82]) + bytes(61) + bytes([0x96])
keepAliveCmd = bytes([0xaa, 0x83]) + bytes(61) + bytes([0x9e])
 
 
def findMeter():
    devices = hid.enumerate(vendorId, productId)
    if not devices:
        sys.exit("FNB58 wasn't found. Check it's plugged in and the FNIRSI software is closed.")
    # print every interface the meter shows up as, so you can check which one you're opening
    for device in devices:
        print("interface", device["interface_number"], "usagePage", device["usage_page"], device["path"])
    return devices[0]["path"]
 
 
def sendCommand(meter, command):
    # hidapi wants a report ID as the first byte. The FNB58 doesn't use report IDs, so it's 0x00.
    meter.write(bytes([0x00]) + command)
 
 
def startStream(meter):
    sendCommand(meter, startCmd1)
    sendCommand(meter, startCmd2)
    sendCommand(meter, startCmd2)   # the FNB58 gets 0x82 twice, as in the reference logger
 
 
def decodePacket(data):
    # Returns a list of 4 (voltageV, currentA) pairs, or None if this isn't a data packet.
    # Layout: byte 0 = 0xaa, byte 1 = packet type (0x04 = data), then 4 samples of 15 bytes each.
    # In each sample: bytes 0-3 voltage, bytes 4-7 current, both little-endian, in units of 0.00001.
    if len(data) != 64 or data[0] != 0xaa or data[1] != 0x04:
        return None
 
    samples = []
    for i in range(4):
        offset = 2 + 15 * i
        voltageV = int.from_bytes(bytes(data[offset:offset + 4]), "little") / 100000
        currentA = int.from_bytes(bytes(data[offset + 4:offset + 8]), "little") / 100000
        samples.append((voltageV, currentA))
    return samples
 
 
def drainMeter(meter):
    # Read until nothing arrives for 1 s, but give up after 5 s at most.
    stopAt = time.monotonic() + 5
    while time.monotonic() < stopAt and meter.read(64, 1000):
        pass

scriptDir = os.path.dirname(os.path.abspath(__file__))
logDir = os.path.join(scriptDir, "..", "_data", "_FNB58")

os.makedirs(logDir, exist_ok=True)
filename = os.path.join(logDir, f"log_FNB58_{time.strftime('%Y-%m-%d_%H-%M-%S')}.csv")

meter = hid.device()
meter.open_path(findMeter())
startStream(meter)

f = open(filename, "w", newline="")
writer = csv.writer(f)
writer.writerow(["voltageV", "currentA", "wallTimeS", "monoTimeS"])

time.sleep(0.1)
 
nextKeepAlive = time.monotonic() + keepAliveEvery
packetCount = 0
rowCount = 0
silentCount = 0
nextFlush = time.perf_counter() + 1
skippedPackets = 0
 
try:
    while True:
        data = meter.read(64, 5000)   # wait up to 5 s for a packet
        arrivalWall = time.time()
        arrivalMono = time.perf_counter()
 
        if not data:
            silentCount += 1
            print(f"{time.strftime('%H:%M:%S')} No data from meter for 5 s")
        else:
            samples = decodePacket(data)
            if samples is None:
                skippedPackets += 1
            else:
                packetCount += 1
                # samples has 4 pairs
                for i in range(4):
                    voltageV, currentA = samples[i] # unpacks into 2 variables
                    secondsAgo = (4 - i) * sampleInterval # when each sample was taken

                    # write the taken voltage, current, wall time, monotonic time (w/ perf_counter)
                    writer.writerow([voltageV, currentA, arrivalWall - secondsAgo, arrivalMono - secondsAgo])
                    rowCount += 1
 
        if time.monotonic() >= nextKeepAlive:
            sendCommand(meter, keepAliveCmd)
            nextKeepAlive = time.monotonic() + keepAliveEvery
 
        if time.perf_counter() >= nextFlush:    # at the end of each loop
            f.flush()
            os.fsync(f.fileno())
            nextFlush += 1
 
except KeyboardInterrupt:
    pass

except OSError as error:
    print(f"{time.strftime('%H:%M:%S')} Meter disconnected: {error}")
 
finally:
    f.close()

    try:
        drainMeter(meter)
    except OSError:
        print("Couldn't drain meter (it was disconnected...)")

    meter.close()

    print(f"stopped, .csv saved to {filename}")
    print(f"Packet count: {packetCount}, Skipped packets: {skippedPackets}, Rows written: {rowCount}, Data missed for 5s: {silentCount}")