import hid
import sys

# declare FNB58's vendor and product IDs
vendorId = 0x2E3C
productId = 21848
devices = hid.enumerate(vendorId, productId)

# print error if it's not there
if not devices:
    print("FNB58 wasn't found. Check if it's plugged in.")
    sys.exit()
else:
    print(devices[0]["path"])