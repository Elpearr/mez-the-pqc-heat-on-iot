# imports
import subprocess
import time
import csv
import os

# write to a .csv
logDir = os.path.expanduser("~/yste/logs")
os.makedirs(logDir, exist_ok=True)
filename = os.path.join(logDir, time.strftime("log_%Y-%m-%d_%H-%M-%S.csv"))

f = open(filename, "w", newline="")
writer = csv.writer(f)
writer.writerow(["tempC", "clockHz", "throttledRaw", "wallTimeS", "monoTimeS"])


# function to gather and parse data
def read_sensors():
    # gather reported measurements from the stdout
    temp = subprocess.run(["vcgencmd", "measure_temp"], capture_output=True, text=True).stdout
    clock = subprocess.run(["vcgencmd", "measure_clock", "arm"], capture_output=True, text=True).stdout
    throttled = subprocess.run(["vcgencmd", "get_throttled"], capture_output=True, text=True).stdout

    # strip results of the unneccessary parts
    temp1 = float(temp.split("=")[1].strip().rstrip("'C"))
    clockHz = int(clock.split("=")[1].strip())
    throttled1 = throttled.split("=")[1].strip()

    return temp1, clockHz, throttled1


nextTime = time.monotonic()
rowCount = 0

try:
    while True:
        currentTime = time.time()
        monotonicTime = time.monotonic()

        temp, clockHz, throttled = read_sensors()

        # display MHz, but log raw hertz
        print(temp, clockHz / 10 ** 6, throttled, currentTime, monotonicTime)

        writer.writerow([temp, clockHz, throttled, currentTime, monotonicTime])
        f.flush()
        os.fsync(f.fileno())

        nextTime = nextTime + 1
        rowCount = rowCount + 1

        time.sleep(max(nextTime - time.monotonic(), 0))
except KeyboardInterrupt:
    print(f"stopped by user, .csv saved to {filename}, rows counted: {rowCount}")

finally:
    f.close()