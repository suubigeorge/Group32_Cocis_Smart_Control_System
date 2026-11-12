# CoCIS Smart Building Control and Monitoring System

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2%2B%20%7C%20v6.1-blue)](https://docs.espressif.com/projects/esp-idf/)
[![FreeRTOS](https://img.shields.io/badge/FreeRTOS-Multi--Tasking-green)](https://www.freertos.org/)
[![Simulator](https://img.shields.io/badge/Wokwi-ESP32%20Simulation-orange)](https://wokwi.com)
[![Coursework](https://img.shields.io/badge/Coursework-BSE%20III%20%2F%20CS%20II%202026%2F2027-purple)](#)

An enterprise-grade embedded building access, automation, and monitoring system implemented on a single **ESP32** microcontroller using **ESP-IDF (C)** and **FreeRTOS**. Designed for Makerere University's College of Computing and Information Sciences (CoCIS), the system manages **three logical terminals** as concurrent FreeRTOS tasks alongside background supervisor, storage, and management services.

---

## Table of Contents

- [System Architecture](#system-architecture)
- [Key Features](#key-features)
- [Hardware & Pin Mapping](#hardware--pin-mapping)
- [FreeRTOS Task Map & Module Ownership](#freertos-task-map--module-ownership)
- [Default RFID Cards & Credentials](#default-rfid-cards--credentials)
- [Build & Run Instructions](#build--run-instructions)
- [Interactive Command Line Interface (CLI)](#interactive-command-line-interface-cli)
- [Step-by-Step Demo Guide](#step-by-step-demo-guide)
- [Project Directory Structure](#project-directory-structure)
- [Team Members & Credits](#team-members--credits)

---

## System Architecture

The ESP32 hosts three logical operational terminals and supporting system tasks:

```
                                  +---------------------------------------+
                                  |         ESP32 Microcontroller         |
                                  +---------------------------------------+
                                                      |
         +--------------------------------------------+--------------------------------------------+
         |                                            |                                            |
         v                                            v                                            v
+-----------------------------------+   +-----------------------------------+   +-----------------------------------+
|     TERMINAL A: Main Entrance     |   |    TERMINAL B: Lecture Room 1     |   |    TERMINAL C: Lecture Room 2     |
| - Dual MFRC522 Readers (SPI3)     |   | - PIR Motion Detection (GPIO 27)  |   | - PIR Motion Detection (GPIO 4)   |
| - Entry & Exit Servos (PWM 13,14) |   | - Ambient LDR Sensing (ADC1_CH6)  |   | - Ambient LDR Sensing (ADC1_CH7)  |
| - Anti-Passback & Security Alarm  |   | - Automated Relay/LED (GPIO 16)   |   | - Automated Relay/LED (GPIO 17)   |
+-----------------------------------+   +-----------------------------------+   +-----------------------------------+
         |                                            |                                            |
         +--------------------------------------------+--------------------------------------------+
                                                      |
                                                      v
                                  +---------------------------------------+
                                  |    BACKGROUND & SUPERVISORY TASKS     |
                                  | - Supervisor (Schedule, LCD Clock)    |
                                  | - Logger (Thread-Safe SD Card Logging)|
                                  | - Console (Admin CLI & Command Parser)|
                                  | - Shared Storage (NVS Configuration)  |
                                  +---------------------------------------+
```

### Key Technical Concepts
- **Concurrency & Synchronization**: All shared state is protected by mutexes (`g_state_mtx`, `g_cfg_mtx`, `g_recent_mtx`). Inter-task communication relies on FreeRTOS queues (`g_log_q`, `g_tap_q`) and event groups (`g_sys_events`).
- **Dual SPI Controllers**: MFRC522 RFID readers operate on dedicated SPI bus 3 (`HSPI`), while the MicroSD card module operates on SPI bus 2 (`VSPI`), preventing MISO bus contention.
- **Data Persistence**: System configurations and user registries persist in Non-Volatile Storage (NVS). Log records are continuously committed to MicroSD card CSV files with RAM fallback.

---

## Key Features

### 🔐 1. Access Control (Terminal A)
- **Dual RFID Reader Gate**: Dedicated MFRC522 readers for entry and exit doors.
- **Anti-Passback Enforcement**: Prevents a card from entering twice without exiting first, maintaining exact building occupancy counts.
- **Role-Based Access Control**: Standard roles include `STUDENT`, `STAFF`, `SECURITY`, and `ADMIN`.
- **Security Alarms & Emergency Release**: Detects unauthorized entry attempts and unknown card bursts (3 invalid scans within 60s trigger buzzer alarm). Physical emergency push button unlocks both doors immediately.

### 💡 2. Smart Room Automation (Terminals B & C)
- **PIR & LDR Co-Optimization**: Automatically controls room lighting based on human motion and natural daylight (LUX levels).
- **Occupancy Timeouts**: Switches lights OFF after 30 seconds of inactivity to save power.
- **Energy Analytics**: Tracks light duration, occupied time, and calculates cumulative energy usage (Watt-hours) and cost.
- **After-Hours Security**: Detects unauthorized motion when the building is closed and sounds alarms.

### ⏱️ 3. Building Supervisor & Real-Time Clock
- **DS1307 Hardware RTC Integration**: Maintains precise system time across power cycles over I2C bus.
- **Schedule Management**: Automatically enforces building opening/closing hours (default `07:00` to `22:00`).
- **Closing Warnings & End-of-Day Audit**: Broadcasts closing alerts (15 minutes prior) and generates occupancy audit reports at closing time.
- **Shared LCD Display**: Displays time, building status, and access events on a 16x2 I2C LCD screen.

### 💾 4. Reliable Storage & Logging
- **MicroSD Card Logging**: Asynchronous, non-blocking queue logger writes event records to `/sdcard/ACCESS.CSV`, `/sdcard/EVENTS.LOG`, and `/sdcard/INSIDE.CSV`.
- **RAM Fallback Buffer**: Keeps the last 40 events in a thread-safe ring buffer if the MicroSD card is removed or unmounted.

### 🖥️ 5. Secure Serial Console CLI
- **Admin Authentication**: Salted SHA-256 password authentication with a 3-strike lockout (60-second cooldown).
- **Full System Control**: Live tracking, user enrollment/revocation, schedule adjustments, manual light overrides, and CSV log exports.

---

## Hardware & Pin Mapping

Below is the pin assignment table matching `config.h` and Wokwi `diagram.json`:

| Hardware Module | Sub-component | Pin / Signal | ESP32 GPIO / Interface | Notes |
|---|---|---|---|---|
| **I2C Shared Bus** | LCD 16x2 (0x27) & DS1307 RTC (0x68) | SDA / SCL | **GPIO 21** / **GPIO 22** | I2C Master, 100 kHz |
| **MicroSD Card (SPI2)** | Storage Module | CS / SCK / MOSI / MISO | **GPIO 5** / **18** / **23** / **19** | FATFS File System |
| **Entrance Door** | Entry Servo Motor | PWM | **GPIO 13** | PWM Control (0° locked, 90° open) |
| **Exit Door** | Exit Servo Motor | PWM | **GPIO 14** | PWM Control (0° locked, 90° open) |
| **RFID Readers (SPI3)** | Dual MFRC522 Readers | SCK / MOSI / MISO / RST | **GPIO 2** / **12** / **26** / **25** | Dedicated SPI3 Bus |
| | Entry RFID Reader (SDA) | CS | **GPIO 32** | Chip Select Entry |
| | Exit RFID Reader (SDA) | CS | **GPIO 33** | Chip Select Exit |
| **Alarms & Controls** | Emergency Push Button | Input | **GPIO 0** | Active Low (BOOT Button) |
| | Active Buzzer | Output | **GPIO 15** | Tone/Beep Alerts |
| **Lecture Room 1** | PIR Motion Sensor | Digital In | **GPIO 27** | Room 1 Occupancy |
| | LDR Light Sensor | Analog In | **GPIO 34** | ADC1 Channel 6 |
| | Lighting LED / Relay | Digital Out | **GPIO 16** | Active High via 220Ω Resistor |
| **Lecture Room 2** | PIR Motion Sensor | Digital In | **GPIO 4** | Room 2 Occupancy |
| | LDR Light Sensor | Analog In | **GPIO 35** | ADC1 Channel 7 |
| | Lighting LED / Relay | Digital Out | **GPIO 17** | Active High via 220Ω Resistor |

---

## FreeRTOS Task Map & Module Ownership

The application creates 7 concurrent FreeRTOS tasks at system startup:

| Task Name | Priority | Stack Size | Function / Feature Description | Module Owner |
|---|---|---|---|---|
| `access` | 6 | 4 KB | Entrance logic, card authorization, door servos, anti-passback | Kuoi David Manyang |
| `rfid` | 5 | 4 KB | Dual MFRC522 polling, UID extraction, tap queuing | Kuoi David Manyang |
| `room1` | 5 | 3 KB | Lecture Room 1 PIR/LDR sensing, light state machine, energy stats | Eria Paul |
| `room2` | 5 | 3 KB | Lecture Room 2 PIR/LDR sensing, light state machine, energy stats | Eria Paul |
| `supervisor` | 4 | 3 KB | Schedule enforcement, closing alerts, occupancy audit, LCD clock | Mugalu Benjamin |
| `logger` | 3 | 4 KB | Async SD Card CSV writer (sole writer task for thread safety) | Mugalu Benjamin |
| `console` | 2 | 6 KB | Interactive serial CLI parser, admin authentication, diagnostics | Ongol Matthew |
| *Core Infra* | - | - | System init, FreeRTOS synchronization objects, hardware drivers | Suubi George |

---

## Default RFID Cards & Credentials

The system initializes default cards into NVS upon first boot:

| Card / Tag Type | UID | Assigned Person | Role | Initial Access Status |
|---|---|---|---|---|
| **Blue Card** | `01:02:03:04` | Kuoi David | STUDENT | Active |
| **Green Card** | `11:22:33:44` | Dr. Lecturer | STAFF | Active |
| **Yellow Card** | `55:66:77:88` | Security Guard | SECURITY | Active |
| **NFC Tag** | `04:11:22:33:44:55:66` | Building Admin | ADMIN | Active |
| **Key Fob** | `C0:FF:EE:99` | Lost Card | - | **Blocked** |
| **Red Card** | `AA:BB:CC:DD` | Unregistered | - | Denied (Triggers Alarm on 3 taps) |

*Additional pre-loaded team cards*: `B2:C3:D4:E5` (Eria), `C3:D4:E5:F6` (Suubi), `D4:E5:F6:A7` (Mugalu), `E5:F6:A7:B8` (Ongol).

- **Default Admin Password**: `admin123` (Changeable via `passwd` CLI command).

---

## Build & Run Instructions

### Prerequisites
- **ESP-IDF** (v5.2 or newer required due to new `driver/i2c_master.h` driver; tested on ESP-IDF v6.1).
- **VS Code** with ESP-IDF extension and Wokwi extension installed.

### Option A: VS Code + Wokwi Simulator (Recommended)

1. Clone the repository and open the project directory in VS Code:
   ```bash
   git clone https://github.com/suubigeorge/Group32_Cocis_Smart_Control_System.git
   cd Group32_Cocis_Smart_Control_System
   ```
2. Set the build target to ESP32:
   ```bash
   idf.py set-target esp32
   ```
3. Build the firmware:
   ```bash
   idf.py build
   ```
4. Start Wokwi simulation:
   - Press `F1` in VS Code -> Select **Wokwi: Start Simulator**.
   - `wokwi.toml` automatically loads the built `.elf` and binary files.

### Option B: Wokwi Online Browser Simulator

1. Go to [wokwi.com](https://wokwi.com) and start a new **ESP32 ESP-IDF project**.
2. Replace the web editor's `diagram.json` with the `diagram.json` from this repository.
3. Upload all source files from `main/` along with `CMakeLists.txt` and `wokwi.toml`.
4. Click **Start Simulation**.

---

## Interactive Command Line Interface (CLI)

Open the serial terminal (115200 baud) to interact with the system console.

### Open Commands (Accessible to All Users)
```bash
help                    # Show command listing
status                  # System health, time, occupancy, doors, lights
readers                 # Report MFRC522 SPI readers hardware status
inside                  # List individuals currently inside the building
rooms                   # Detailed status of Lecture Rooms 1 & 2
energy                  # Report power metrics (lit hours, occupancy, kWh cost)
users                   # Display registered RFID cards database
log [n]                 # Print last n lines from RAM event buffer (default 20)
sdlog <access|events>   # Print last n lines from SD card CSV logs
schedule                # Print current building operating hours
time                    # Display current DS1307 RTC and system timestamp
tap <in|out> <uid>      # Simulate an RFID card scan over serial console
login                   # Authenticate as Administrator
logout                  # End Admin session
whoami                  # Show current terminal privilege level
```

### Admin Commands (Requires `login` with Admin Password)
```bash
setsched <HH:MM> <HH:MM>              # Change building open/close operating hours
warn <minutes>                         # Set closing warning period (default 15 mins)
settime <YYYY-MM-DD> <HH:MM[:SS]>      # Manually set system RTC date and time
adduser <uid> <student|staff|security|admin> <Name>  # Register a new card
deluser <uid>                          # Remove a user card from NVS
block <uid> / unblock <uid>            # Freeze or unfreeze card access
checkout <uid>                         # Force manual checkout of a card user
light <1|2> <auto|on|off>              # Manual lighting override for Room 1 or 2
timeout <seconds>                      # Set PIR occupancy timeout (default 30s)
ldr <0-4095>                           # Adjust daylight light threshold
emergency <on|off>                     # Toggle building emergency override
lockdown <on|off>                      # Toggle lockdown (refuses normal entries)
export                                 # Export current inside occupancy snapshot to SD card
passwd                                 # Change administrator password
```

---

## Step-by-Step Demo Guide

Follow this 5-minute verification script to test all core subsystems:

1. **System Startup & Health Check**:
   - Start the simulation. Observe LCD displaying `"CoCIS Smart Ctrl Starting..."` followed by the clock and system status.
   - Type `status` in the console to verify all tasks are operational.
   - If RTC time requires synchronization, execute: `login` -> `settime 2026-11-02 09:00`.

2. **Access Control & Anti-Passback Test**:
   - Execute `readers` to confirm both entry and exit MFRC522 readers are online.
   - Select the **Blue Card** on the **ENTRY Reader** in Wokwi and click **Tap**.
   - *Result*: Entry Servo rotates to 90° (OPEN), LCD displays `"WELCOME Kuoi David"`, buzzer beeps.
   - Tap the Blue Card on the ENTRY Reader again.
   - *Result*: Access denied (`"Already inside"` - Anti-Passback active).
   - Tap the Blue Card on the **EXIT Reader**.
   - *Result*: Exit Servo opens, user marked as exited.

3. **Security Alarms & Intrusions**:
   - Tap the **Key Fob** (`C0:FF:EE:99`) on the ENTRY Reader.
   - *Result*: Denied (`"Card blocked"`).
   - Tap the **Red Card** (`AA:BB:CC:DD`) on the ENTRY Reader 3 times rapidly.
   - *Result*: System sounds continuous alarm tone and logs an intrusion alert.

4. **Occupancy & Smart Lighting Automation**:
   - Click on **PIR 1** (Motion sensor for Room 1).
   - *Result*: Room 1 LED turns ON (LDR default is dark 50 Lux).
   - Increase the **LDR 1** slider to 1000 Lux (Bright daylight).
   - *Result*: Light automatically turns OFF (`"enough daylight"`).
   - Set LDR 1 back to 50 Lux and stop PIR motion for 30 seconds.
   - *Result*: PIR timeout expires, room marked `EMPTY`, light turns OFF.
   - Type `rooms` and `energy` in the console to view power analytics.

5. **Schedule Enforcement & Building Closing**:
   - Run `setsched 07:00 09:01` while the system time is `09:02`.
   - *Result*: Building transitions to **CLOSED**.
   - Tap the Blue Card on the ENTRY Reader. Access denied (`"BUILDING CLOSED"`).
   - Tap the **Yellow Card** (Security Guard). Access granted (Security role bypasses closure).

6. **Emergency Override**:
   - Click the red **EMERGENCY Pushbutton** (GPIO 0).
   - *Result*: Both Entry and Exit Servos switch to 90° (OPEN), both room lights illuminate, LCD indicates emergency mode.
   - Press the button again to return to normal secure operation.

7. **Data Persistence & SD Card Logs**:
   - Type `log 10` to view real-time events stored in the RAM buffer.
   - Type `sdlog access 10` and `sdlog events 10` to review CSV logs saved on the MicroSD card.
   - Execute `export` to generate `/sdcard/INSIDE.CSV`.

---

## Project Directory Structure

```
Group32_Cocis_Smart_Control_System/
├── CMakeLists.txt              # Root CMake build configuration
├── README.md                   # System documentation & manual
├── LICENSE                     # Open-source license file
├── diagram.json                # Wokwi circuit topology & component layout
├── wokwi.toml                  # Wokwi simulator launch configuration
├── sdkconfig.ci                # Continuous Integration SDK config
└── main/
    ├── CMakeLists.txt          # Main component build rules
    ├── config.h                # System constants, pin definitions, and defaults
    ├── app_state.h / .c        # Shared state, thread safety mutexes & event groups
    ├── access.h / .c           # Terminal A: RFID readers, door servos & anti-passback
    ├── rooms.h / .c            # Terminals B & C: PIR/LDR occupancy & smart lighting
    ├── console.h / .c          # Interactive serial CLI, admin commands & login
    ├── schedule.h / .c         # Building opening/closing schedule & supervisor task
    ├── storage.h / .c          # NVS storage & async MicroSD card queue logger
    ├── rtc_ds1307.h / .c       # DS1307 I2C Real-Time Clock driver
    ├── lcd_i2c.h / .c          # PCF8574 I2C 16x2 LCD display driver
    ├── rc522.h / .c            # MFRC522 SPI RFID reader driver
    ├── hw.h / .c               # Hardware abstraction (GPIO, ADC, PWM, Buzzer, I2C)
    └── main.c                  # System entry point, startup sequence & FreeRTOS task map
```

---

## Team Members & Credits

Developed by **Group 32** for the Embedded Systems Coursework 2026/2027, Department of Computer Science / Software Engineering, Makerere University CoCIS:

| Team Member | Reg / Student ID | Primary Project Module & Responsibility |
|---|---|---|
| **Suubi George** | *Lead Developer* | System Architecture, FreeRTOS Synchronization, Core Hardware Drivers (`main.c`, `app_state`, `hw`) |
| **Kuoi David Manyang** | *Developer* | Terminal A: Access Control Logic, Dual RFID Readers & Anti-Passback (`access.c`, `rc522.c`) |
| **Eria Paul** | *Developer* | Terminals B & C: Smart Classroom Occupancy, Lighting & Energy Analytics (`rooms.c`) |
| **Mugalu Benjamin** | *Developer* | Storage Subsystems, NVS Management, MicroSD Logging & DS1307 RTC (`storage.c`, `rtc_ds1307.c`, `schedule.c`) |
| **Ongol Matthew** | *Developer* | Serial Console CLI, Admin Authentication & I2C LCD Interface (`console.c`, `lcd_i2c.c`) |

---
*College of Computing and Information Sciences (CoCIS) — Makerere University*
