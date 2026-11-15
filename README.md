# Micro Matter Button

A tiny, coin-cell-powered **Matter over Thread** smart home button built on the [HolyIoT 25008](https://s.click.aliexpress.com/e/_c4MbjoMv) and [HolyIoT 25015](https://s.click.aliexpress.com/e/_c3R5B6Fn) Bluetooth beacon boards. Both boards use the Nordic nRF54l15 SoC — a chip capable of running Matter and Zigbee, making it perfect for a DIY wireless smart home button.

This project was built for a YouTube build video. See the full step-by-step on my [YouTube channel](https://www.youtube.com/@uamphome)

[![Alt Text](assets/images/micro_matter_button.png)](assets/images/micro_matter_button.png)

> **No soldering required.** Flash the precompiled binary, commission it to your smart home, and you're done.

---

## Hardware

| Item | Notes |
|------|-------|
| [HolyIoT 25008](https://s.click.aliexpress.com/e/_c4MbjoMv) | Core board — button only |
| [HolyIoT 25015](https://s.click.aliexpress.com/e/_c3R5B6Fn) | Adds SHT40 temperature & humidity sensor |
| [Seeed Studio Xiao RP2040](https://s.click.aliexpress.com/e/_c3o6kaAl) | Used as a CMSIS-DAP programmer. via picoprobe firmware. Any Raspberry PI pico board with the RP2040 or RP2350 will do |
| [PCB spring-pin test clip](https://s.click.aliexpress.com/e/_c39zxHAZ) | Clamps to the board's SWD pads — no soldering needed. 4 Pins. 1.5MM Pitch |
| CR2032 coin cell battery | Powers the finished button |

The **25015** is a drop-in hardware upgrade over the 25008. It uses the same firmware but additionally reports temperature and humidity to your smart home hub. Both boards run off a single coin cell.

---

## Quick Start — Precompiled Binaries

> **Start here.** You do not need to install the nRF Connect SDK or compile anything.

### Step 1 — Download the right binary

Four precompiled binaries are provided in the [Releases](../../releases/latest) section:

| File | Board | Mode |
|------|-------|------|
| `micro_matter_button_h25008_om1.hex` | HolyIoT 25008 | Mode 1 — Dimmable switch (requires binding) |
| `micro_matter_button_h25008_om2.hex` | HolyIoT 25008 | Mode 2 — Generic switch |
| `micro_matter_button_h25015_om1.hex` | HolyIoT 25015 | Mode 1 — Dimmable switch (requires binding) |
| `micro_matter_button_h25015_om2.hex` | HolyIoT 25015 | Mode 2 — Generic switch |

**Not sure which mode to pick?** Read the [Operating Modes](#operating-modes) section below, then come back.

### Step 2 — Set up the programmer

The HolyIoT boards have no USB port. You flash them over SWD using a Raspberry Pi Pico running picoprobe firmware.

[![Alt Text](assets/images/setup.png)](assets/images/setup.png)

1. Hold the **BOOTSEL** button on the Pico and plug it into your computer.
2. Download the [picoprobe UF2](https://github.com/raspberrypi/picoprobe/releases) and drag it onto the Pico — it will appear as a USB drive.
3. Wire the Pico to the HolyIoT board's SWD pads through the PCB test clip:

   | Pico Pin | Arduino Nano RP2040 Connect | HolyIoT Pad |
   |----------|-----------------------------|-------------|
   | GP2 (SWCLK) | D10 (GPIO 2) | SWCLK |
   | GP3 (SWDIO) | D11 (GPIO 3) | SWDIO |
   | GND | GND | GND |
   | 3V3 | 3V3 | VCC (optional — or use the coin cell) |

<!-- INSERT PICTURE: PCB test clip attached to HolyIoT board sitting in 3D-printed programming jig, wired to Pico -->

### Step 3 — Flash with pyocd

Install pyocd and its nRF54l pack:

```bash
pip install pyocd
```

Flash the firmware (replace the filename with your chosen binary):

```bash
pyocd load ./holyiot_25008_mode1.hex -t nrf54l
```

That's it — the chip will reset and start advertising over Bluetooth LE.

### Step 4 — Commission to your smart home

Open your smart home app and add a new Matter device. Use the QR code or manual pairing code below when prompted.



**Manual pairing code:** `34970112332`  
**QR Code:** [Scan or screenshot this](https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT:4CT9142C00KA0648G00)

> The LED on the board will blink slowly while waiting for commissioning and turn off once paired.

---

## Operating Modes & Wake-on-Turn Dimming

When mounted **vertically on a wall**, the LIS2DH12 accelerometer stays in a **10 Hz Low-Power Wake-on-Turn mode (~3 µA)** with its high-pass motion interrupt (`INT1`) enabled.
- **No double-tap needed:** Simply turn the knob on the wall—`INT1` wakes the MCU immediately and streams live relative brightness steps every 150 ms while you rotate.
- After 2.5 seconds of inactivity, the accelerometer returns to 10 Hz low-power sleep automatically.

### Home Assistant Integration (Endpoint 2 — Generic Switch Events)

Both Mode 1 and Mode 2 expose **Endpoint 2 (`Switch` cluster)** to Home Assistant so you can control any Home Assistant light (such as a **Gledopto** controller, Zigbee/Wi-Fi bulb, or light group) while keeping full state overview in Home Assistant:

| Gesture | Home Assistant `event_type` | Event Attribute | Action |
|---------|----------------------------|-----------------|--------|
| Single click | `short_release` | `previous_position: 1` | Toggle light on/off |
| Turn clockwise (default) | `long_press` | `new_position: 1..100` | Step **brightness up** by `new_position` % |
| Turn counter-clockwise (default) | `long_release` | `previous_position: 1..100` | Step **brightness down** by `previous_position` % |
| **Double-click** + turn clockwise | `long_press` | `new_position: 101..200` | Step **color temperature cooler (K)** by `(new_position - 100)` % |
| **Double-click** + turn counter-clockwise | `long_release` | `previous_position: 101..200` | Step **color temperature warmer (K)** by `(previous_position - 100)` % |
| **Swipe vertically UP** (in hand) | `short_release` | `previous_position: 201` | **Open / raise** roller shutters (`cover.open_cover`) |
| **Swipe vertically DOWN** (in hand) | `short_release` | `previous_position: 202` | **Close / lower** roller shutters (`cover.close_cover`) |
| **Swipe HORIZONTALLY** (any direction) | `short_release` | `previous_position: 203` | **Stop** roller shutters (`cover.stop_cover`) |

*(After double-clicking and adjusting color temperature, stopping rotation for 2.5 seconds automatically returns the knob to Brightness mode for the next turn. Linear swipes use 3D gravity projection so they work regardless of how the round magnetic button is rotated in your hand.)*

#### Example Home Assistant Automation (Gledopto Light + Roller Shutters)

```yaml
alias: "Micro Matter Button - Gledopto Light & Roller Shutters"
mode: queued
max: 20
trigger:
  - platform: state
    entity_id: event.matter_light_switch_button
    not_from:
      - unavailable
      - unknown
variables:
  ev_type: "{{ states('event.matter_light_switch_button') }}"
  raw_pos: >-
    {{ state_attr('event.matter_light_switch_button',
       'new_position' if ev_type == 'long_press' else 'previous_position') | int(1) }}
  is_color_temp: "{{ 101 <= raw_pos <= 200 }}"
  step_pct: "{{ (raw_pos - 100) if is_color_temp else raw_pos }}"
  signed_pct: "{{ step_pct if ev_type == 'long_press' else -step_pct }}"
  min_k: "{{ state_attr('light.gledopto', 'min_color_temp_kelvin') | int(2000) }}"
  max_k: "{{ state_attr('light.gledopto', 'max_color_temp_kelvin') | int(6500) }}"
  cur_k: "{{ state_attr('light.gledopto', 'color_temp_kelvin') | int(3500) }}"
action:
  - choose:
      # 1. Single click -> Toggle light
      - conditions:
          - condition: template
            value_template: "{{ ev_type == 'short_release' and raw_pos == 1 }}"
        sequence:
          - action: light.toggle
            target:
              entity_id: light.gledopto

      # 2. Swipe UP -> Open roller shutters
      - conditions:
          - condition: template
            value_template: "{{ ev_type == 'short_release' and raw_pos == 201 }}"
        sequence:
          - action: cover.open_cover
            target:
              entity_id: cover.roller_shutters

      # 3. Swipe DOWN -> Close roller shutters
      - conditions:
          - condition: template
            value_template: "{{ ev_type == 'short_release' and raw_pos == 202 }}"
        sequence:
          - action: cover.close_cover
            target:
              entity_id: cover.roller_shutters

      # 4. Swipe HORIZONTAL (any direction) -> Stop roller shutters
      - conditions:
          - condition: template
            value_template: "{{ ev_type == 'short_release' and raw_pos == 203 }}"
        sequence:
          - action: cover.stop_cover
            target:
              entity_id: cover.roller_shutters

      # 5. Rotate (default) -> Step Brightness
      - conditions:
          - condition: template
            value_template: "{{ ev_type in ['long_press', 'long_release'] and not is_color_temp }}"
        sequence:
          - action: light.turn_on
            target:
              entity_id: light.gledopto
            data:
              brightness_step_pct: "{{ signed_pct }}"
              transition: 0.2

      # 6. Double-click + Rotate -> Step Color Temperature (Kelvin)
      - conditions:
          - condition: template
            value_template: "{{ ev_type in ['long_press', 'long_release'] and is_color_temp }}"
        sequence:
          - action: light.turn_on
            target:
              entity_id: light.gledopto
            data:
              color_temp_kelvin: >-
                {{ [max_k, [min_k, (cur_k + (signed_pct * (max_k - min_k) / 100)) | int] | max] | min }}
              transition: 0.2
```

---

### Mode 1 vs. Mode 2

- **Mode 1 (`om1`):** Enables **both** Endpoint 2 (Home Assistant `Switch` events above) **and** Endpoint 1 (`LightSwitch` Matter Binding client using relative `StepWithOnOff` commands).
- **Mode 2 (`om2`):** Enables Endpoint 2 (Home Assistant `Switch` events with rotational dimming) and disables Endpoint 1 for hubs that prefer a pure Generic Switch device.

---

## Setting Up Binding (Mode 1 Only)

Binding is a Matter concept that tells the switch which light(s) to control. Without it, the button has no target to send commands to.

### With Home Assistant (recommended)

Home Assistant makes binding straightforward, especially when running on a VM or alongside a Thread Border Router. I have a dedicated video on my YouTube channel walking through how to share Matter devices across fabrics and set up bindings in Home Assistant — [watch it here](https://www.youtube.com/watch?v=eK-TLgXYBtU).

### With chip-tool (advanced)

If you prefer the command line, use chip-tool to write the binding table.

---

## Battery Life

Average current consumption is approximately **6 µA** — achieved by:

- Running as a Thread Sleepy End Device (SED) with long poll intervals
- Powering down the LIS2DH12 accelerometer at boot (Mode 2) or on demand (Mode 1)
- Keeping all debug/serial peripherals disabled in the production build
- Link-time optimisation (LTO) enabled

A standard CR2032 (≈220 mAh capacity) gives a rough estimate of **~36,000 hours / ~4 years** of theoretical runtime. Real-world life will be lower due to battery self-discharge and voltage droop, but multi-year life off a single coin cell is realistic.

---

## 3D Printed Case

<!-- INSERT PICTURE: Side-by-side of plain case and grooved case designs -->

STL files for the 3D printed case are available in the `assets/stl/` directory. The design is two parts that screw together — the board sits in the base and the top half has a flexible plunger that transmits button presses through the face of the case. Prints cleanly in PLA with no post-processing.

Two versions are provided:
- `micro_matter_button_25008.stl` - Holyiot 25008
- `micro_matter_button_25015.stl` - Holyiot 25015
- `programming_jig.stl` - Jig to hold the Holyiot Board during programming

---

## HolyIoT 25015 — Temperature & Humidity Variant

The 25015 uses the same firmware binaries but adds two extra sensors:

- **SHT40** — temperature and humidity (fully working, reported every 5 minutes after commissioning)
- **LPS22HB** — barometric pressure (not yet implemented — the driver in NCS assumes I²C; the board uses SPI. The sensor is explicitly powered down at boot.)

The 25015 case has a small airflow hole near the sensor for accurate readings.

---

## Building from Source

For developers who want to customise the firmware.

### Prerequisites

- [nRF Connect SDK v3.x](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/installation.html) (v3.2.4 used for the precompiled binaries)
- West build tools

### Selecting a mode at build time

The operating mode is a Kconfig choice. Pass it on the west build command line:

```bash
# Mode 1 — Dimmable switch
west build -b holyiot_25008_nrf54l15_cpuapp -- -DCONFIG_MATTER_BUTTON_MODE_1=y

# Mode 2 — Generic switch
west build -b holyiot_25008_nrf54l15_cpuapp -- -DCONFIG_MATTER_BUTTON_MODE_2=y
```

Replace `holyiot_25008` with `holyiot_25015` for the sensor variant.

### KConfig options

| Option | Default | Description |
|--------|---------|-------------|
| `MATTER_BUTTON_MODE_1` | y | Dimmable switch with accelerometer-based rotational dimming |
| `MATTER_BUTTON_MODE_2` | n | Generic switch with short-press and long-press events |

Both options are mutually exclusive — only one may be set at a time.

---

## Factory Reset

Hold the button for **12 seconds**. The LED will blink white as a warning. Release before 15 seconds to cancel. After 15 seconds the device resets and re-enters commissioning mode.

---

## License

Source code is licensed under the [LicenseRef-Nordic-5-Clause](LICENSE) licence, consistent with the nRF Connect SDK samples this project is based on.

---

*Built and documented for the [YouTube channel](https://www.youtube.com/@uamphome). Full build guide and video linked below.*
