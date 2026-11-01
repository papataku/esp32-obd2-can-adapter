#!/usr/bin/env python3
"""Explicit, one-shot USB ARM or drained STOP for the owner-live image.

No port discovery, reset, KeepAlive, renewal or automatic ARM is performed.
V2 central registration, subscriptions and its 3s KeepAlive remain required.
"""
import argparse
import sys
import time


def command(port, action, *, serial_factory=None, output=None, timeout=8.0):
    if action not in ("arm", "stop"):
        raise ValueError("action must be arm or stop")
    if output is None:
        output = sys.stdout
    if serial_factory is None:
        import serial
        serial_factory = serial.Serial
    # Configure control lines before opening: do not deliberately reset board.
    conn = serial_factory(port=None, baudrate=115200, timeout=0.2,
                          write_timeout=1, exclusive=True)
    try:
        conn.dtr = False
        conn.rts = False
        conn.port = port
        conn.open()
        conn.reset_input_buffer()  # Discard acknowledgements from earlier commands.
        conn.write(("SIGV2 CAN " + action.upper() + "\n").encode("ascii"))
        conn.flush()
        requested = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = conn.readline().decode("ascii", errors="replace").strip()
            # Never print private SIGV2 PEERS/identity output or raw CAN logs.
            if not line.startswith("#SIGNALV2_CAN,"):
                continue
            print(line, file=output, flush=True)
            fields = set(line.split(",")[1:])
            if action == "arm":
                if "armed=1" in fields and "reason=USB_VERIFIED" in fields:
                    return 0
                if "armed=0" in fields and "ack=STOPPED" not in fields:
                    return 1
            elif "ack=REQUESTED" in fields and "reason=OPERATOR_STOP" in fields:
                requested = True
            elif requested and "ack=STOPPED" in fields:
                return 0
        print("ACK timeout: state unconfirmed; verify LCD and send STOP.",
              file=output, flush=True)
        return 1
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="owner-verified USB port")
    parser.add_argument("action", choices=("arm", "stop"))
    args = parser.parse_args()
    try:
        return command(args.port, args.action)
    except (OSError, ImportError) as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
