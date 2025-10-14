# WSL2 USB Device Setup for STM32 Nucleo

## 🔍 **Issue Identified**

You're running in **WSL2** (Windows Subsystem for Linux 2), which doesn't have direct USB access. USB devices need to be explicitly shared from Windows to WSL2.

### Current Status:
```
✅ lsusb installed
✅ Container configured for USB
❌ No STM32 device visible
❌ USB devices not passed through from Windows
```

---

## 🛠️ **Solution: Install usbipd-win on Windows**

### **Step 1: Install usbipd-win on Windows HOST**

This tool allows sharing USB devices from Windows to WSL2.

#### **Option A: Using winget (Recommended)**
Open **PowerShell or CMD** as Administrator on Windows:
```powershell
winget install --interactive --exact dorssel.usbipd-win
```

#### **Option B: Manual Download**
1. Go to: https://github.com/dorssel/usbipd-win/releases
2. Download latest `.msi` installer
3. Run installer as Administrator
4. Restart your computer

---

## 🔌 **Attach Your Nucleo Board to WSL2**

### **Step 2: Connect Nucleo and Find Device ID**

1. **Plug in your Nucleo board** to Windows PC (Mini-USB to CN1)

2. **Open PowerShell as Administrator** on Windows

3. **List USB devices:**
```powershell
usbipd list
```

You should see something like:
```
BUSID  VID:PID    DEVICE                                  STATE
1-4    0483:374b  STMicroelectronics STLink dongle        Not shared
2-1    046d:c52b  Logitech USB Input Device              Not shared
...
```

**Look for:**
- `0483:374b` - STMicroelectronics ST-LINK
- Or device name containing "STM" or "STLink"

4. **Note the BUSID** (e.g., `1-4`)

---

### **Step 3: Bind and Attach Device**

In **PowerShell as Administrator** on Windows:

```powershell
# Replace 1-4 with your actual BUSID
usbipd bind --busid 1-4

# Attach to WSL2
usbipd attach --wsl --busid 1-4
```

**Expected output:**
```
usbipd: info: Device with busid '1-4' attached to WSL.
```

---

### **Step 4: Verify in WSL2 Container**

Back in your **VS Code WSL2 terminal**:

```bash
# Check if ST-LINK appears now
lsusb | grep STM

# Should show:
# Bus 001 Device 002: ID 0483:374b STMicroelectronics ST-LINK/V2.1
```

```bash
# Check serial port
ls -la /dev/ttyACM*

# Should show:
# crw-rw---- 1 root dialout 166, 0 Oct 9 10:00 /dev/ttyACM0
```

```bash
# Test ST-LINK connection
st-info --probe

# Should show your STM32F439ZI details
```

---

## 🔄 **Persistent USB Attachment (Recommended)**

The USB attachment is **lost when you disconnect or reboot**. For automatic reattachment:

### **Option A: Auto-attach on connection (PowerShell script)**

Create this script on Windows: `C:\Scripts\attach-nucleo.ps1`

```powershell
# attach-nucleo.ps1
$BUSID = "1-4"  # Change to your Nucleo's BUSID

Write-Host "Attaching Nucleo board to WSL2..."
usbipd attach --wsl --busid $BUSID

if ($?) {
    Write-Host "✅ Nucleo attached successfully!"
} else {
    Write-Host "❌ Failed to attach. Check BUSID and ensure board is connected."
}
```

Run this script whenever you connect your board.

### **Option B: Create Windows shortcut**

1. Right-click Desktop → New → Shortcut
2. Location: `powershell.exe -ExecutionPolicy Bypass -File C:\Scripts\attach-nucleo.ps1`
3. Name: "Attach Nucleo to WSL"
4. Right-click shortcut → Properties → Advanced → Run as administrator

---

## 📋 **Complete Workflow**

### **Every time you connect Nucleo:**

1. **Plug Nucleo** into Windows PC
2. **Run PowerShell as Admin**
3. **Attach to WSL:**
   ```powershell
   usbipd attach --wsl --busid 1-4
   ```
4. **In WSL2 terminal:**
   ```bash
   # Verify
   lsusb | grep STM
   
   # Flash firmware
   st-flash write /workspaces/rusefi/firmware/build/rusefi.bin 0x8000000
   ```

### **After flashing:**

```bash
# Check serial port
ls -la /dev/ttyACM0

# Connect TunerStudio
# Port: /dev/ttyACM0
# Baud: 115200
```

---

## 🐛 **Troubleshooting**

### **Issue: "usbipd: command not found"**
**Solution:** Install usbipd-win on Windows (see Step 1)

### **Issue: "Access denied" when running usbipd**
**Solution:** Run PowerShell as Administrator
- Right-click PowerShell → "Run as Administrator"

### **Issue: Device appears in Windows but not WSL**
**Solution:** 
```powershell
# Detach first
usbipd detach --busid 1-4

# Wait 2 seconds, then reattach
usbipd attach --wsl --busid 1-4
```

### **Issue: "BUSID not found"**
**Solution:**
```powershell
# List all devices
usbipd list

# Verify Nucleo is plugged in
# Check Device Manager in Windows
```

### **Issue: Device disconnects randomly**
**Solution:**
- USB cable issue - try different cable
- USB port issue - try different port
- Power management - disable USB selective suspend:
  1. Windows Settings → System → Power
  2. Additional power settings → Change plan settings
  3. Change advanced power settings
  4. USB settings → USB selective suspend → Disabled

### **Issue: "Device is not shared"**
**Solution:**
```powershell
# Bind the device first
usbipd bind --busid 1-4

# Then attach
usbipd attach --wsl --busid 1-4
```

### **Issue: /dev/ttyACM0 permission denied**
**Solution:**
```bash
# Add user to dialout group
sudo usermod -a -G dialout $USER

# Logout and login, or restart container
```

---

## 📝 **Quick Reference Commands**

### **Windows PowerShell (as Administrator):**
```powershell
# List USB devices
usbipd list

# Bind device (one-time setup)
usbipd bind --busid 1-4

# Attach to WSL2
usbipd attach --wsl --busid 1-4

# Detach from WSL2
usbipd detach --busid 1-4

# Check device status
usbipd list
```

### **WSL2 Terminal:**
```bash
# Check if device visible
lsusb | grep STM

# Check serial port
ls -la /dev/ttyACM*

# Test ST-LINK
st-info --probe

# Flash firmware
st-flash write firmware.bin 0x8000000

# Connect to serial
screen /dev/ttyACM0 115200
```

---

## ⚡ **Alternative: Use Windows Tools Directly**

If WSL2 USB is too complex, you can flash from Windows directly:

### **Install ST-Link Utility on Windows:**
1. Download: https://www.st.com/en/development-tools/stsw-link004.html
2. Install ST-Link Utility
3. Connect Nucleo
4. File → Open → Select `rusefi.hex`
5. Target → Program & Verify

### **Or use STM32CubeProgrammer on Windows:**
1. Download: https://www.st.com/en/development-tools/stm32cubeprog.html
2. Install STM32CubeProgrammer
3. Connect via ST-LINK
4. Load `rusefi.hex`
5. Download to device

Then use TunerStudio on Windows with the COM port.

---

## ✅ **Setup Checklist**

**On Windows:**
- [ ] Install usbipd-win
- [ ] Restart computer
- [ ] Connect Nucleo board
- [ ] Open PowerShell as Admin
- [ ] Run `usbipd list` to find BUSID
- [ ] Run `usbipd bind --busid X-X`
- [ ] Run `usbipd attach --wsl --busid X-X`

**In WSL2:**
- [ ] Run `lsusb | grep STM` - should see device
- [ ] Run `ls -la /dev/ttyACM0` - should see serial port
- [ ] Run `st-info --probe` - should connect
- [ ] Flash firmware with st-flash
- [ ] Connect TunerStudio

---

## 🎯 **Recommended Approach**

For **simplest workflow**:

1. **Use usbipd-win** for flashing firmware from WSL2
2. **Use TunerStudio on Windows** for daily tuning (no USB passthrough needed)
3. **Create PowerShell shortcut** for quick USB attachment

This gives you the best of both worlds:
- ✅ Build firmware in WSL2 (fast, Linux tools)
- ✅ Flash firmware in WSL2 (after USB attachment)
- ✅ Tune in TunerStudio on Windows (native, no WSL complexity)

---

## 📚 **Resources**

- usbipd-win GitHub: https://github.com/dorssel/usbipd-win
- Microsoft WSL USB Guide: https://learn.microsoft.com/en-us/windows/wsl/connect-usb
- ST-Link Windows Tools: https://www.st.com/en/development-tools/st-link-v2.html

---

**Ready to set up USB passthrough? Start with installing usbipd-win on Windows!** 🚀
