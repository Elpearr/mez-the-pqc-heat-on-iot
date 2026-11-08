# loggingv2.py
# logs the cpu temperature, cpu clock speed, throttle state,
#   timing, and probe temperature readings

# All code written by me (Callum), with bug fixing and 
#   help provided by Claude

# imports
import subprocess
import time
import csv
import os
import socket
import sys
import signal

# if SIGTERM signal -> ensures that except/finally runs even if kill/pkill are used, 
# so it takes the signal and instead prersents it as a keyboardinterrupt instead
def handleStop(signum, frame):
    raise KeyboardInterrupt

signal.signal(signal.SIGTERM, handleStop)
signal.signal(signal.SIGHUP, handleStop)

# constants
insideProbeId = "28-000000dae6eb"
ambientProbeId = "28-000000d1e62b"
w1Dir = "/sys/bus/w1/devices"
piName = socket.gethostname()

def checkResolution(probeID):
    # check what resolution bit the probe is measuring in; 
    # 12 -> 0.0625C @ ~750ms, 11 -> 0.125C @ ~375ms, 10 -> 0.25C @ ~188ms

    path = f"{w1Dir}/{probeID}/resolution"

    try:
        with open(path) as probeResFile:
            probeRes = probeResFile.read().strip()

    except (OSError):
        sys.exit(f"Couldn't find the resolution file for {probeID}.")

    if probeRes != "10":
        sys.exit(f"{probeID}'s resolution is not 10. Set it to 10 with: \"echo 10 | sudo tee /sys/bus/w1/devices/{probeID}/resolution\"")

# now check probes:
checkResolution(insideProbeId)
checkResolution(ambientProbeId)

# write to a .csv
logDir = os.path.expanduser("~/yste/logs")
os.makedirs(logDir, exist_ok=True)
filename = os.path.join(logDir, f"log_{piName}_{time.strftime('%Y-%m-%d_%H-%M-%S')}.csv")

f = open(filename, "w", newline="")
writer = csv.writer(f)
writer.writerow(["chipC", "clockHz", "throttledRaw", "wallTimeS", "monoTimeS", "insideC", "ambientC"])

def runVcgencmd(args):
    try:
        # timeout=2 throws an error if the logger ever hangs on waiting for a value >2s
        result = subprocess.run(["vcgencmd"] + args, capture_output=True, text=True, timeout=2)
        # strip unnecessary parts, we want just raw data
        return result.stdout.split("=")[1].strip()
    except (OSError, IndexError, subprocess.TimeoutExpired):
        return ""
    
# function to gather and parse the temperature, clock speed, and throttle state of the pi
def readSensors():
    # gather reported measurements from the stdout
    temp = runVcgencmd(["measure_temp"])              # subprocess.run(["vcgencmd", "measure_temp"], capture_output=True, text=True).stdout
    clock = runVcgencmd(["measure_clock", "arm"])     # subprocess.run(["vcgencmd", "measure_clock", "arm"], capture_output=True, text=True).stdout
    throttled = runVcgencmd(["get_throttled"])        # subprocess.run(["vcgencmd", "get_throttled"], capture_output=True, text=True).stdout

    # strip results of the unneccessary parts
    try:
        temp1 = float(temp.rstrip("'C"))
    except ValueError:
        temp1 = ""

    try:
        clockHz = int(clock)
    except ValueError:
        clockHz = ""

    return temp1, clockHz, throttled

# function to get the readings from the DS18B20 probes
def readProbe(probeID):
    # probes temperature file path
    path = f"{w1Dir}/{probeID}/temperature"

    try:
        # opens and auto closes
        with open(path) as probeFile:
            # probes return values in millidegree celsius
            milliC = int(probeFile.read().strip())

    except (OSError, ValueError):
        return ""

    # divide to convert to regular celsius from milidegree celsius
    tempC = milliC / 1000

    if tempC == 85.0:
        return ""
    return tempC

nextTime = time.monotonic()
rowCount = 0
probeErrors = 0
sensorErrors = 0
overruns = 0

# main loop
try:
    while True:
        currentTime = time.time()
        monotonicTime = time.monotonic()

        temp, clockHz, throttled = readSensors()

        insideC = readProbe(insideProbeId)
        ambientC = readProbe(ambientProbeId)

        # convert to MHz for display, unless the clock read failed (cant divide empty string)
        if clockHz != "":
            clockMHz = clockHz / 10 ** 6
        else:
            clockMHz = ""

        print(temp, clockMHz, throttled, currentTime, monotonicTime, insideC, ambientC)

        writer.writerow([temp, clockHz, throttled, currentTime, monotonicTime, insideC, ambientC])
        f.flush()
        os.fsync(f.fileno())

        nextTime = nextTime + 1
        rowCount = rowCount + 1

        waitTime = nextTime - time.monotonic()

        if waitTime < 0:
            overruns += 1
            nextTime = time.monotonic()

        sensorErrors += [temp, clockHz, throttled].count("")
        probeErrors += [insideC, ambientC].count("")

        time.sleep(max(waitTime, 0))

except KeyboardInterrupt:
    pass

finally:
    f.close()
    print(f"stopped, .csv saved to {filename}")
    print(f"rows: {rowCount}, probe errors: {probeErrors}, sensor errors: {sensorErrors}, overruns: {overruns}")