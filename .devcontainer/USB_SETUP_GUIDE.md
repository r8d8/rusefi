# Dev Container USB Setup - Complete Guide

## ✅ Changes Made

Your devcontainer has been configured for USB device access:

### 1. **devcontainer.json** - Already configured!
```json
// USB device access for flashing and debugging
"runArgs": [
    "--privileged",
    "--device=/dev/bus/usb"
],

// Mount USB devices into container
"mounts": [
    "source=/dev/bus/usb,target=/dev/bus/usb,type=bind"
]
```

### 2. **Dockerfile** - Updated!
Added USB and serial tools:
```dockerfile
# USB and serial communication tools
RUN apt-get -y install usbutils stlink-tools openocd libusb-1.0-0 libusb-1.0-0-dev dfu-util screen minicom
```

**Installed tools:**
- `usbutils` - Provides `lsusb` command
- `stlink-tools` - ST-Link programmer (`st-flash`, `st-info`)
- `openocd` - Open On-Chip Debugger
- `libusb` - USB library support
- `dfu-util` - DFU mode flashing
- `screen`, `minicom` - Serial terminal tools

---

## 🔄 Rebuild Container

To apply these changes, you need to rebuild the dev container:

### **Method 1: VS Code Command Palette (Recommended)**
1. Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on Mac)
2. Type: `Dev Containers: Rebuild Container`
3. Select it and wait for rebuild (3-5 minutes)
4. Container will restart with USB support

### **Method 2: VS Code UI**
1. Click the blue/green remote icon in bottom-left corner
2. Select `Rebuild Container`
3. Wait for rebuild to complete

### **Method 3: Command Line**
```bash
# From your host machine (not in container)
docker-compose -f .devcontainer/docker-compose.yml down
docker-compose -f .devcontainer/docker-compose.yml build --no-cache
docker-compose -f .devcontainer/docker-compose.yml up -d
```

---

## ✅ Verify USB Access After Rebuild

After the container rebuilds, verify USB access:

### **1. Check USB devices:**
```bash
lsusb
# Should show your connected USB devices
```

### **2. Check for ST-LINK:**
```bash
lsusb | grep -i "stm\|st-link"
# Should show: STMicroelectronics ST-LINK...
```

### **3. Check serial ports:**
```bash
ls -la /dev/ttyACM* /dev/ttyUSB*
# Should show serial ports if Nucleo is connected
```

### **4. Test ST-Link connection:**
```bash
st-info --probe
# Should show your STM32F439ZI details
```

---

## 🔌 Connect Your Nucleo Board

### **Before Rebuilding:**
1. **Disconnect** Nucleo from your PC
2. Wait for container rebuild to complete
3. **Reconnect** Nucleo to PC

### **After Rebuilding:**
1. Connect Nucleo board via Mini-USB to CN1
2. Run in container terminal:
```bash
# Check USB device
lsusb | grep STM

# Check ST-Link
st-info --probe

# Check serial port
ls -la /dev/ttyACM0
```

Expected output:
```
Bus 001 Device 005: ID 0483:374b STMicroelectronics ST-LINK/V2.1
crw-rw---- 1 root dialout 166, 0 Oct  9 10:00 /dev/ttyACM0
```

---

## 📝 Flash Firmware After USB Setup

Once USB is working:

```bash
# Navigate to firmware directory
cd /workspaces/rusefi/firmware

# Flash using st-flash
st-flash write build/rusefi.bin 0x8000000

# Or use OpenOCD
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/rusefi.hex verify reset exit"
```

---

## 🔧 TunerStudio Connection

### **After firmware is flashed:**

1. **Find serial port:**
```bash
ls -la /dev/ttyACM*
# Usually: /dev/ttyACM0
```

2. **Check port permissions:**
```bash
ls -la /dev/ttyACM0
# If permission denied, add user to dialout group:
sudo usermod -a -G dialout $(whoami)
# Then restart container
```

3. **Test serial connection:**
```bash
# Quick test with screen
screen /dev/ttyACM0 115200
# Press Ctrl+A then K to exit
```

4. **Connect TunerStudio:**
   - Port: `/dev/ttyACM0`
   - Baud: `115200`
   - Load: `tunerstudio/generated/rusefi_mre_f4.ini`

---

## 🐛 Troubleshooting

### **Issue: lsusb still not found after rebuild**
```bash
# Check if rebuild actually happened
docker images | grep rusefi

# Manually install in running container (temporary):
apt-get update && apt-get install -y usbutils stlink-tools

# Then rebuild properly for permanent fix
```

### **Issue: No USB devices visible in lsusb**
```bash
# Check host machine first
# Exit container and run on host:
lsusb | grep STM

# If visible on host but not in container:
# 1. Make sure container has --privileged flag
# 2. Check /dev/bus/usb is mounted
ls -la /dev/bus/usb/
```

### **Issue: Permission denied on /dev/ttyACM0**
```bash
# Add user to dialout group
sudo usermod -a -G dialout $(whoami)

# Or run with sudo (not recommended for daily use)
sudo st-flash write build/rusefi.bin 0x8000000
```

### **Issue: st-flash not found**
```bash
# Install manually if needed
apt-get update
apt-get install -y stlink-tools

# Verify installation
which st-flash
st-flash --version
```

### **Issue: Container doesn't see newly connected USB**
```bash
# USB hotplug doesn't always work in containers
# Solution: Reconnect device
# 1. Unplug Nucleo
# 2. Wait 2 seconds
# 3. Plug back in
# 4. Check: lsusb
```

---

## 📋 Quick Reference Commands

### **USB Detection:**
```bash
lsusb                              # List all USB devices
lsusb -v -d 0483:374b             # Detailed ST-LINK info
ls -la /dev/ttyACM*               # List serial ports
dmesg | grep tty                  # Check USB connection logs
```

### **ST-Link Commands:**
```bash
st-info --probe                   # Probe ST-LINK connection
st-info --chipid                  # Read chip ID
st-flash --version                # Check st-flash version
st-flash read out.bin 0x8000000 0x10000  # Read flash
st-flash write firmware.bin 0x8000000    # Write flash
st-flash erase                    # Erase chip
```

### **Serial Port Commands:**
```bash
screen /dev/ttyACM0 115200        # Connect with screen
minicom -D /dev/ttyACM0 -b 115200 # Connect with minicom
cat /dev/ttyACM0                  # View raw output
echo "help" > /dev/ttyACM0        # Send command
```

### **OpenOCD Commands:**
```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg  # Start server
openocd -c "program firmware.hex verify reset exit"     # Flash and exit
```

---

## ✅ Checklist: Container Rebuild Steps

Before rebuilding:
- [ ] Save and commit any important work
- [ ] Disconnect Nucleo board
- [ ] Close all open files

During rebuild:
- [ ] Run "Rebuild Container" from VS Code
- [ ] Wait 3-5 minutes for rebuild
- [ ] Container restarts automatically

After rebuild:
- [ ] Reconnect Nucleo board
- [ ] Test: `lsusb` shows STM32
- [ ] Test: `st-info --probe` works
- [ ] Test: `/dev/ttyACM0` exists
- [ ] Flash firmware with st-flash
- [ ] Connect TunerStudio

---

## 🚀 Next Steps

1. **Rebuild container** (Ctrl+Shift+P → "Rebuild Container")
2. **Wait for rebuild** to complete
3. **Connect Nucleo** via USB
4. **Verify USB access** with `lsusb`
5. **Flash firmware** with `st-flash`
6. **Connect TunerStudio** to `/dev/ttyACM0`
7. **Configure servo** throttle mode
8. **Test!** 🎉

---

**After rebuilding, you'll have full USB access for flashing and debugging!**
