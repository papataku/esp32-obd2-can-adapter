"""Run host-only simulated ELM327 PTY or the opt-in Virtual-only CDC bridge.

Neither mode uses SocketCAN, TWAI, a CAN controller or a vehicle.
"""
import argparse
import os
import select
import sys
import time
from . import ECUSimulator, ELM327Session, load_model
from .vcan import VirtualCDCBridge


def elm_pty(model):
    import pty
    import tty
    master, slave = pty.openpty()
    tty.setraw(slave)
    print("ELM PTY available at " + os.ttyname(slave), file=sys.stderr, flush=True)
    session = ELM327Session(model)
    buffer = bytearray()
    while True:
        try:
            data = os.read(master, 1024)
        except (OSError, KeyboardInterrupt):
            break
        if not data:
            break
        for byte in data:
            if byte == 13:
                try:
                    response = session.request(buffer.decode("ascii"))
                except (ValueError, UnicodeDecodeError):
                    response = b"?\r>"
                os.write(master, response)
                buffer.clear()
            elif byte == 10 and not buffer:
                pass
            elif len(buffer) >= 96:
                buffer.clear()
                os.write(master, b"?\r>")
            else:
                buffer.append(byte)


def virtual_cdc(model, port, confirmed, baudrate):
    if not confirmed or os.environ.get("TTM_VIRTUAL_ONLY_ACK") != "YES":
        raise SystemExit("Refused: --virtual-only-ack plus TTM_VIRTUAL_ONLY_ACK=YES required")
    if not port or not port.startswith("/dev/"):
        raise SystemExit("Refused: explicit /dev/ USB CDC path required")
    import serial
    bridge = VirtualCDCBridge(ECUSimulator(model))
    start = time.monotonic_ns()
    pending = bytearray()
    with serial.Serial(port, baudrate=baudrate, timeout=0, write_timeout=0.5) as device:
        while True:
            try:
                ready, _, _ = select.select([device.fileno()], [], [], 0.01)
            except KeyboardInterrupt:
                return
            now_us = (time.monotonic_ns() - start) // 1000
            if ready:
                for byte in device.read(128):
                    pending.append(byte)
                    if len(pending) > 64:
                        raise SystemExit("Virtual CDC overflow, abort; no CAN fallback")
                    if byte == 0:
                        try:
                            result = bridge.handle(bytes(pending))
                        except ValueError as exc:
                            raise SystemExit(f"Virtual CDC framing/identity failure: {exc}")
                        pending.clear()
                        for response in result:
                            device.write(response)
            for response in bridge.tick(now_us):
                device.write(response)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Host-only ELM/Virtual CAN simulator")
    parser.add_argument("--model", required=True, help="strict anonymized ECU model JSON")
    parser.add_argument("--mode", choices=("elm-pty", "virtual-cdc"), default="elm-pty")
    parser.add_argument("--port", help="explicit USB CDC device for Virtual-only FW")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--virtual-only-ack", action="store_true")
    args = parser.parse_args(argv)
    model = load_model(args.model)
    if args.mode == "elm-pty":
        if args.port or args.virtual_only_ack:
            parser.error("elm-pty mode has no physical port")
        elm_pty(model)
    else:
        virtual_cdc(model, args.port, args.virtual_only_ack, args.baudrate)


if __name__ == "__main__":
    main()
