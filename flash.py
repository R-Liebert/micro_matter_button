#!/usr/bin/env python3
"""Fast continuous SWD detector and instant in-process flasher for HolyIoT nRF54L15 buttons."""

from __future__ import annotations

from pathlib import Path
import sys
import time

from pyocd.core.helpers import ConnectHelper
from pyocd.core.session import Session
from pyocd.flash.file_programmer import FileProgrammer
from pyocd.probe.cmsis_dap_probe import CMSISDAPProbe
from pyocd.probe.debug_probe import DebugProbe
from pyocd.probe.swj import SWJSequenceSender


def main() -> int:
    """Poll the SWD probe at 50 Hz using pyOCD's SWJ sequences and flash on contact."""
    check_only = "--check" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--check"]
    btn_num = int(args[0]) if args else 2
    repo_dir = Path(__file__).resolve().parent
    hex_path = repo_dir / "build_hex" / "variants" / f"Egen_superknapp_{btn_num:02d}.hex"
    if not check_only and not hex_path.exists():
        print(f"Error: {hex_path} not found.")
        return 1

    if check_only:
        print("=== Read-Only 50 Hz SWD Detector (will NOT flash or erase) ===")
    else:
        print(f"=== Fast 50 Hz Auto-Flasher armed for Button {btn_num:02d} ({hex_path.name}) ===")
    print("Hold the pogo clip on the pads. Probing 50 times/sec...")

    probe = None
    swj = None
    attempt = 0
    use_dormant = False

    while True:
        attempt += 1
        try:
            if probe is None:
                probes = CMSISDAPProbe.get_all_connected_probes()
                if not probes:
                    sys.stdout.write(f"\r[{attempt:05d}] [USB] Waiting for Nano RP2040 Connect probe...          ")
                    sys.stdout.flush()
                    time.sleep(0.2)
                    continue
                probe = probes[0]
                Session(probe, options={"target_override": "nrf54l"})
                probe.open()
                probe.connect(DebugProbe.Protocol.SWD)
                probe.set_clock(400000)
                swj = SWJSequenceSender(probe, use_dormant=False)

            swj.use_dormant = use_dormant
            use_dormant = not use_dormant
            swj.select_protocol(DebugProbe.Protocol.SWD)
            dpidr = probe.read_dp(0x0, now=True)

            if check_only:
                print(f"\a\n>>> READ-ONLY TARGET LOCKED! DPIDR = 0x{dpidr:08X} <<<")
                probe.close()
                return 0

            print(f"\a\n>>> TARGET LOCKED! DPIDR = 0x{dpidr:08X} — HOLD STEADY, FLASHING NOW... <<<")
            probe.close()
            probe = None

            with ConnectHelper.session_with_chosen_probe(
                target_override="nrf54l",
                frequency=1000000,
                blocking=False,
            ) as session:
                FileProgrammer(session).program(str(hex_path))
                session.target.reset()

            print(f"\a\n=== SUCCESS! {hex_path.name} FLASHED AND RESET! ===")
            return 0

        except KeyboardInterrupt:
            print("\nStopped by user.")
            if probe is not None:
                try:
                    probe.close()
                except Exception:
                    pass
            return 130
        except Exception as exc:
            msg = str(exc)
            if "Unexpected ACK" in msg:
                status = "[ACK=0] SWDIO is LOW (0V) — check D10/D11 swap or VCC contact   "
            elif "No ACK" in msg:
                status = "[No ACK] Open circuit — press pogo pins onto the 4 pads...      "
            else:
                status = f"[{type(exc).__name__}] Reconnecting probe...                         "
                if probe is not None:
                    try:
                        probe.close()
                    except Exception:
                        pass
                    probe = None
                    swj = None
            sys.stdout.write(f"\r[{attempt:05d}] {status}")
            sys.stdout.flush()
            time.sleep(0.015)


if __name__ == "__main__":
    raise SystemExit(main())
