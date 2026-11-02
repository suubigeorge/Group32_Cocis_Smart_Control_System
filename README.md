# CoCIS Smart Control & Monitoring System

**Makerere University**  
**College of Computing and Information Sciences (CoCIS)**  
**Department of Computer Science & Software Engineering**  
*BSE III & CS II Embedded Systems Coursework (2026/2027)*  
**Group 32 Project**

---

## 📋 Table of Contents
1. [Executive Summary](#-executive-summary)
2. [System Architecture](#-system-architecture)
3. [Hardware & Peripheral Wiring](#-hardware--peripheral-wiring)
4. [Software Design & FreeRTOS Tasks](#-software-design--freertos-tasks)
5. [Key System Features](#-key-system-features)
6. [Interactive CLI Commands](#-interactive-cli-commands)
7. [Storage & Data Logging](#-storage--data-logging)
8. [Build & Simulation Guide](#-build--simulation-guide)
9. [Pre-Registered Demo Cards](#-pre-registered-demo-cards)
10. [Team Members](#-team-members)

---

## 🔬 Executive Summary

The **CoCIS Smart Control & Monitoring System** is an enterprise-grade embedded building management system (BMS) implemented on the **ESP32** microcontroller using **ESP-IDF v5.x** and **FreeRTOS**. 

Designed specifically for academic buildings at CoCIS, Makerere University, the system unifies three logical terminal nodes into a single, highly synchronized ESP32 device:

*   **Terminal A (Main Entrance):** RFID-based dual-door access control (Entry & Exit turnstiles), strict anti-passback policy enforcement, role-based entry permissions, emergency fire release, security lockdown mode, and intruder intrusion alarms.
*   **Terminal B (Lecture Room 1):** Motion-aware (PIR) and daylight-harvesting (LDR) automated lighting system with real-time energy usage and savings metrics calculation.
*   **Terminal C (Lecture Room 2):** Independent secondary lecture room environment controller with after-hours motion detection alerts.

The system features real-time schedule enforcement (backed by an external **DS1307 RTC** over I2C), non-volatile system configuration & user card storage (**NVS Flash**), asynchronous log auditing on a **MicroSD Card** (SPI / FATFS), an **I2C 16x2 LCD Display**, and a password-protected serial **CLI Console**.

---

## 📐 System Architecture

The project is built around an event-driven, task-based multitasking architecture on FreeRTOS. Tasks communicate strictly via thread-safe primitives (mutexes, FreeRTOS event groups, and FreeRTOS queues) to eliminate race conditions and deadlocks.

```
+-----------------------------------------------------------------------------------+
|                                  ESP32 MCU                                        |
|                                                                                   |
|  +------------------+  +------------------+  +------------------+  +-----------+  |
|  |  access_task     |  |   room_task (1)  |  |   room_task (2)  |  |supervisor |  |
|  |  (Terminal A)    |  |   (Terminal B)   |  |   (Terminal C)   |  |   task    |  |
|  |  Prio: 6         |  |   Prio: 5        |  |   Prio: 5        |  |  Prio: 4  |  |
|  +--------+---------+  +--------+---------+  +--------+---------+  +-----+-----+  |
|           |                     |                     |                |          |
|           +-----------+---------+-----------+---------+                |          |
|                       |                     |                          |          |
|                       v                     v                          v          |
|              +---------------------------------------------------+                |
|              | FreeRTOS Event Group (g_sys_events)               |                |
|              |   - EVT_BUILDING_OPEN     - EVT_EMERGENCY         |                |
|              |   - EVT_CLOSING_SOON      - EVT_LOCKDOWN          |                |
|              |   - EVT_SD_OK                                     |                |
|              +-------------------------+-------------------------+                |
|                                        |                                          |
|                                        v                                          |
|  +-----------------------------------------------------------------------------+  |
|  |                     g_log_q (Non-blocking Queue)                            |  |
|  +-------------------------------------+---------------------------------------+  |
|                                        |                                          |
|                                        v                                          |
|  +-----------------------------------------------------------------------------+  |
|  | logger_task (Prio: 3) -> Writes SD Card (ACCESS.CSV & EVENTS.LOG)           |  |
|  +-----------------------------------------------------------------------------+  |
|                                                                                   |
|  +-----------------------------------------------------------------------------+  |
|  | console_task (Prio: 2) -> Admin Authentication & Interactive CLI Menu       |  |
|  +-----------------------------------------------------------------------------+  |
+-----------------------------------------------------------------------------------+
```

---

## 🔌 Hardware & Peripheral Wiring

The Wokwi simulation model (`diagram.json`) maps all peripherals to ESP32 GPIOs as defined in `main/config.h`:

### Pin Mapping Table

| Component / Subsystem | Peripheral / Signal | ESP32 Pin | Logic / Protocol | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **I2C Bus** | SDA | **GPIO 21** | I2C Master | Shared bus for LCD & RTC |
| **I2C Bus** | SCL | **GPIO 22** | I2C Master | Pull-ups enabled |
| **LCD 16x2 Display** | PCF8574 Backpack | `0x27` | I2C | Real-time status / Messages |
| **DS1307 RTC** | Real-Time Clock | `0x68` | I2C | Date/Time retention |
| **MicroSD Card** | SPI CS | **GPIO 5** | SPI | Chip Select |
| **MicroSD Card** | SPI SCK | **GPIO 18** | SPI | Clock |
| **MicroSD Card** | SPI MOSI (DI) | **GPIO 23** | SPI | Master Out |
| **MicroSD Card** | SPI MISO (DO) | **GPIO 19** | SPI | Master In |
| **Terminal A Doors** | Servo Entry PWM | **GPIO 13** | LEDC PWM (50Hz) | Turnstile / Door Lock |
| **Terminal A Doors** | Servo Exit PWM | **GPIO 14** | LEDC PWM (50Hz) | Turnstile / Door Lock |
| **Terminal A Inputs**| Entry Tap Button | **GPIO 32** | Active Low (Pullup) | Tap card on Entry reader |
| **Terminal A Inputs**| Exit Tap Button | **GPIO 33** | Active Low (Pullup) | Tap card on Exit reader |
| **Terminal A Inputs**| Card Select Button| **GPIO 25** | Active Low (Pullup) | Cycle through demo cards |
| **Terminal A Inputs**| Emergency Button | **GPIO 26** | Active Low (Pullup) | Fire alarm toggle switch |
| **Acoustic Output** | Buzzer PWM | **GPIO 15** | LEDC PWM (2kHz) | Audio feedback & alarms |
| **Lecture Room 1** | PIR Motion Sensor | **GPIO 27** | Digital Input | Motion detector |
| **Lecture Room 1** | LDR Light Sensor | **GPIO 34** | ADC1 Channel 6 | Ambient lux level |
| **Lecture Room 1** | LED Lighting | **GPIO 16** | Digital Output | Relay / Light fixture driver |
| **Lecture Room 2** | PIR Motion Sensor | **GPIO 4** | Digital Input | Motion detector |
| **Lecture Room 2** | LDR Light Sensor | **GPIO 35** | ADC1 Channel 7 | Ambient lux level |
| **Lecture Room 2** | LED Lighting | **GPIO 17** | Digital Output | Relay / Light fixture driver |

---

## 🛠️ Software Design & FreeRTOS Tasks

The software implementation follows modular C programming standards under ESP-IDF v5.x:

1.  **`access_task` (Priority 6):**
    *   Handles Terminal A access control.
    *   Listens to card taps (from physical pushbuttons or console `tap` commands queued in `g_tap_q`).
    *   Enforces Anti-Passback, Access Schedule Hours, Card Activation Status, Role Authorization, Emergency Releases, and Lockdown states.
    *   Triggers servo motor angles (`0°` locked, `90°` open) and acoustic buzzer chimes.
    *   Detects burst invalid cards (3 unknown card taps within 60s) to trigger an intrusion alert.

2.  **`room_task` (Priority 5, Dual Instances):**
    *   Two tasks manage Terminals B & C independently.
    *   Reads PIR occupancy and samples LDR ambient light via ADC1.
    *   Maintains an occupancy timer (`DEFAULT_PIR_TIMEOUT_S = 30s`).
    *   Drives room lighting automatically (`AUTO` mode: lights on only if occupied AND dark). Supports override modes (`FORCED_ON`, `FORCED_OFF`).
    *   Tracks Wh energy consumption and power savings when lights remain off during daylight or empty periods.
    *   Raises intruder alerts when motion is detected during building closed hours.

3.  **`supervisor_task` (Priority 4):**
    *   Monitors real-time clock time against configured building access hours (`DEFAULT_OPEN_HHMM = 07:00`, `DEFAULT_CLOSE_HHMM = 22:00`).
    *   Sets or clears `EVT_BUILDING_OPEN` in the global event group.
    *   Triggers closing warnings (`15 min` prior to closing) and broadcasts audio/LCD alerts.
    *   Logs security alerts if occupants remain inside after closing hours.
    *   Updates the main LCD idle screen with time, building status, occupancy count, and selected demo card.

4.  **`logger_task` (Priority 3):**
    *   Single consumer task processing events from `g_log_q`.
    *   Writes structured CSV records to `/sdcard/ACCESS.CSV` and `/sdcard/EVENTS.LOG`.
    *   Maintains a 40-event RAM ring buffer for immediate viewing on the console.
    *   Zero-wait producer design ensures door access evaluation never stalls if the SD card is busy or missing.

5.  **`console_task` (Priority 2):**
    *   Provides an interactive command-line interface over UART Serial (115200 baud).
    *   Implements role-based access control with admin login, password salt + SHA-256 hashing (PSA Crypto / MbedTLS), brute-force lockout (3 failures = 60s lockout), and 5-minute session timeout.

---

## ✨ Key System Features

*   **Anti-Passback Protection:** Prevents card sharing by ensuring a user who is inside cannot tap "IN" again without first tapping "OUT".
*   **Role-Based Security:** Card roles include `STUDENT`, `STAFF`, `SECURITY`, and `ADMIN`. Security and Admin personnel enjoy after-hours and lockdown access privileges.
*   **Emergency Mode (Fire Alarm):** Toggling Emergency mode immediately releases both entry/exit servos, illuminates all lecture room lighting, sounds an alarm chime, and logs the incident.
*   **Lockdown Mode:** Instantly restricts entry to non-privileged cards during security incidents.
*   **Daylight Harvesting & Energy Tracking:** Calculates exact lighting power consumption (Watts) and energy saved (Watt-hours) based on room wattage configuration (`DEFAULT_ROOM_WATTS = 400W`).
*   **Encrypted Admin Authentication:** Admin passwords are stored in NVS as salted SHA-256 hashes (`psa_hash_compute`). Plain-text passwords are never saved.

---

## 💻 Interactive CLI Commands

Connect to the ESP32 serial monitor at **115200 baud**. Type `help` or `menu` to display the command menu.

### 👁️ View Commands (Accessible to Guest / Anyone)

| Command | Syntax | Description |
| :--- | :--- | :--- |
| `status` | `status` | Displays system status, heap memory, time, occupancy, and room states. |
| `inside` | `inside` | Lists all users currently checked inside the building with entry timestamps. |
| `rooms` | `rooms` | Displays motion, occupancy status, LDR lux values, and light modes for all rooms. |
| `energy` | `energy` | Reports total energy used (Wh) and saved (Wh) per room since boot. |
| `users` | `users` | Displays all registered RFID cards, roles, active status, and location. |
| `log` | `log [n]` | Prints the last `n` events stored in the RAM ring buffer (default 15). |
| `sdlog` | `sdlog access\|events [n]` | Displays the last `n` lines from `/sdcard/ACCESS.CSV` or `/sdcard/EVENTS.LOG`. |
| `schedule` | `schedule` | Shows current building operating hours and closing warning lead time. |
| `time` | `time` | Displays the current DS1307 real-time clock date and time. |
| `tap` | `tap in\|out <uid>` | Simulates tapping an RFID card with `uid` at the Entry or Exit reader. |
| `login` | `login` | Prompts for admin password to unlock administrative privilege. |
| `logout` | `logout` | Ends the administrative session. |
| `whoami` | `whoami` | Shows current session privilege (`admin` or `guest`). |

### 🔑 Admin Commands (Requires Admin Login)

| Command | Syntax | Description |
| :--- | :--- | :--- |
| `setsched` | `setsched HH:MM HH:MM` | Configures building open and close hours (e.g. `setsched 07:00 22:00`). |
| `warn` | `warn <min>` | Sets closing warning lead time in minutes (0-120 min). |
| `settime` | `settime YYYY-MM-DD HH:MM[:SS]` | Sets the DS1307 RTC and system clock. |
| `adduser` | `adduser <uid> <role> <name...>` | Registers or updates a card (`student`, `staff`, `security`, `admin`). |
| `deluser` | `deluser <uid>` | Deletes an RFID card from NVS storage. |
| `block` | `block <uid>` | Deactivates/blocks an RFID card. |
| `unblock` | `unblock <uid>` | Reactivates a blocked RFID card. |
| `checkout` | `checkout <uid>` | Manually marks a person as checked outside (administrative record correction). |
| `light` | `light <1\|2> <auto\|on\|off>` | Sets room lighting mode (`auto`, `on`, `off`). |
| `timeout` | `timeout <5-3600>` | Configures PIR vacancy delay timeout in seconds. |
| `ldr` | `ldr <0-4095>` | Sets the LDR darkness threshold (raw ADC value). |
| `emergency`| `emergency on\|off` | Activates or clears emergency fire evacuation mode. |
| `lockdown` | `lockdown on\|off` | Activates or clears building security lockdown. |
| `export` | `export` | Generates a snapshot of current building occupants to `/sdcard/INSIDE.CSV`. |
| `passwd` | `passwd` | Updates the admin password (saved as salted SHA-256 in NVS). |

---

## 💾 Storage & Data Logging

The system uses a hybrid persistence model:

### 1. Non-Volatile Storage (NVS Flash)
*   **System Configuration (`sys_config_t`):** Stores operating schedule, PIR delay timeouts, LDR darkness thresholds, room wattage, password salt, and hashed password.
*   **User Registry (`user_rec_t`):** Stores up to 32 user card records (UID, cardholder name, role, active status).

### 2. MicroSD Card (FATFS File System)
*   `/sdcard/ACCESS.CSV`: Log of all entry and exit events (`timestamp,result,direction,uid,name,detail`).
*   `/sdcard/EVENTS.LOG`: System-wide audit trail for room automation, configuration changes, warnings, and alerts (`timestamp,type,detail`).
*   `/sdcard/INSIDE.CSV`: Exported CSV snapshot listing occupants inside the building when `export` is executed.

---

## 🚀 Build & Simulation Guide

### Prerequisites
*   **ESP-IDF v5.x** installed and configured in your environment.
*   **Wokwi Simulator** (VS Code Extension or Web Browser).

### Building with ESP-IDF
```bash
# Set target MCU
idf.py set-target esp32

# Build the firmware
idf.py build

# Flash firmware to physical board (optional)
idf.py -p COM_PORT flash monitor
```

### Running in Wokwi Simulation
1. Ensure `wokwi.toml` points to the compiled binary:
   ```toml
   [wokwi]
   version = 1
   firmware = 'build/flasher_args.json'
   elf = 'build/Group32_Cocis_Smart_Control_System.elf'
   ```
2. Open `diagram.json` in VS Code with the Wokwi extension installed.
3. Click **Start Simulation** (Play button).
4. Use the Wokwi pushbuttons to simulate card taps and card selection:
   * **CARD select (Yellow - GPIO 25):** Cycles through demo RFID cards.
   * **ENTRY tap (Green - GPIO 32):** Taps the selected card at the Entry turnstile.
   * **EXIT tap (Blue - GPIO 33):** Taps the selected card at the Exit turnstile.
   * **EMERGENCY (Red - GPIO 26):** Toggles fire emergency release.

---

## 🎴 Pre-Registered Demo Cards

On initial boot, if no cards are saved in NVS, the system automatically loads 9 default demo cards:

| Card UID | Cardholder Name | Role | Initial Status |
| :--- | :--- | :--- | :--- |
| `A1B2C3D4` | Kuoi David | Student | Active |
| `B2C3D4E5` | Eria Paul | Student | Active |
| `C3D4E5F6` | Suubi George | Student | Active |
| `D4E5F6A7` | Mugalu Benjamin | Student | Active |
| `E5F6A7B8` | Ongol Matthew | Student | Active |
| `11223344` | Dr. Lecturer | Staff | Active |
| `55667788` | Security Guard | Security | Active |
| `99AABBCC` | Building Admin | Admin | Active |
| `DEADBEEF` | Lost Card | Student | **Blocked** (Demo blocked card) |
| `0BADCAFE` | *Unregistered* | N/A | *Unknown Card* |

---

## 👥 Team Members

**Group 32** — Makerere University, CoCIS  
*   **Kuoi David**
*   **Eria Paul**
*   **Suubi George**
*   **Mugalu Benjamin**
*   **Ongol Matthew**

---

### 📄 License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
