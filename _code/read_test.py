import subprocess

# gather reported measurements from the stdout
temp = subprocess.run(["vcgencmd", "measure_temp"], capture_output=True, text=True).stdout
clock = subprocess.run(["vcgencmd", "measure_clock", "arm"], capture_output=True, text=True).stdout
throttled = subprocess.run(["vcgencmd", "get_throttled"], capture_output=True, text=True).stdout

# strip results of the unneccessary parts
temp1 = float(temp.split("=")[1].strip().rstrip("'C"))
clock_hz = int(clock.split("=")[1].strip())
throttled1 = throttled.split("=")[1].strip()

print(temp1, clock_hz / 10 ** 6, throttled1) # display MHz, but log raw hertz