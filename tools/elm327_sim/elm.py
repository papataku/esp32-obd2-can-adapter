"""Minimal ELM327 host emulator for deterministic conformance tests.

Subset intentionally explicit: unsupported commands return '?' (never fake OK).
The implementation never transmits CAN or opens any hardware interface.
"""
from __future__ import annotations
from .can import CANFrame, ECUSimulator
from .model import ECUModel


def _print_hex(data: bytes, spaces: bool) -> str:
    return (" " if spaces else "").join(f"{b:02X}" for b in data)


class ELM327Session:
    def __init__(self, model: ECUModel):
        self.bus = ECUSimulator(model)
        self.last = ""
        self.reset()

    def reset(self):
        self.echo = True
        self.linefeed = False
        self.spaces = True
        self.headers = False
        self.protocol = 0
        self.autoformat = True
        self.flowcontrol = True
        self.adaptive = 1
        self.timeout_4ms = 0x32
        self.can_priority = 0x18
        self.header_11 = 0x7DF
        self.header_29 = 0x18DB33F1
        self.monitoring = False

    @staticmethod
    def _normalized(command: str):
        return "".join(command.upper().split())

    def _format_line(self, line: str) -> bytes:
        return line.encode("ascii") + (b"\r\n" if self.linefeed else b"\r")

    def request(self, command: str) -> bytes:
        """Send one command as if CR terminated; returns response through '>'."""
        if not isinstance(command, str) or len(command) > 96 or not command.isascii():
            raise ValueError("bounded ASCII command required")
        typed = command.rstrip("\r\n")
        original = self._normalized(typed)
        if not original:
            original = self.last
        else:
            self.last = original
        echo = self.echo
        if self.monitoring:
            self.monitoring = False
            lines = ["STOPPED"]
        else:
            lines = self._execute(original)
        result = (self._format_line(typed) if echo else b"")
        result += b"".join(self._format_line(line) for line in lines if line is not None)
        return result + b">"

    def _execute(self, cmd: str) -> list[str]:
        if cmd in ("ATZ", "ATWS"):
            self.reset()
            return ["ELM327 v2.2"]
        if cmd == "ATD":
            self.reset()
            return ["OK"]
        if cmd == "ATI":
            return ["ELM327 v2.2"]
        if cmd == "AT@1":
            return ["OBDII to RS232 Interpreter"]
        if cmd == "ATDP":
            if self.protocol == 0:
                return ["AUTO, ISO 15765-4 (CAN 29/500)"]
            size = 11 if self.protocol in (6, 8) else 29
            rate = 500 if self.protocol in (6, 7) else 250
            return [f"ISO 15765-4 (CAN {size}/{rate})"]
        if cmd == "ATDPN":
            return ["A7" if self.protocol == 0 else str(self.protocol)]
        for prefix, attribute in (
            ("ATE", "echo"), ("ATL", "linefeed"), ("ATS", "spaces"),
            ("ATH", "headers"), ("ATCAF", "autoformat"), ("ATCFC", "flowcontrol"),
        ):
            if cmd in (prefix + "0", prefix + "1"):
                setattr(self, attribute, cmd[-1] == "1")
                return ["OK"]
        if cmd in ("ATAL", "ATNL"):
            return ["OK"]
        if cmd in ("ATAT0", "ATAT1", "ATAT2"):
            self.adaptive = int(cmd[-1])
            return ["OK"]
        if cmd.startswith("ATST") and len(cmd) == 6:
            try:
                self.timeout_4ms = int(cmd[4:], 16)
            except ValueError:
                return ["?"]
            return ["OK"]
        if cmd in ("ATSP0", "ATSP6", "ATSP7", "ATSP8", "ATSP9"):
            self.protocol = int(cmd[-1])
            return ["OK"]
        if cmd.startswith("ATCP") and len(cmd) == 6:
            try:
                priority = int(cmd[4:], 16)
            except ValueError:
                return ["?"]
            if priority > 0x1F:
                return ["?"]
            self.can_priority = priority
            return ["OK"]
        if cmd.startswith("ATSH"):
            value = cmd[4:]
            try:
                n = int(value, 16)
            except ValueError:
                return ["?"]
            if len(value) == 3 and n <= 0x7FF:
                self.header_11 = n
                return ["OK"]
            if len(value) == 6 and n <= 0xFFFFFF:
                self.header_29 = (self.can_priority << 24) | n
                return ["OK"]
            if len(value) == 8 and n <= 0x1FFFFFFF:
                self.header_29 = n
                return ["OK"]
            return ["?"]
        if cmd == "ATPC":
            self.protocol = 0
            return ["OK"]
        if cmd == "ATMA":
            # Nonblocking real-time monitor belongs to a future dedicated tier.
            self.monitoring = True
            return []  # The synchronous request API returns the test prompt only.
        if not cmd.startswith("AT") and len(cmd) in (4, 6):
            try:
                request = bytes.fromhex(cmd)
            except ValueError:
                return ["?"]
            if ((len(request) == 2 and request[0] == 1) or
                (len(request) == 3 and request[0] == 0x22)):
                return self._obd(request)
        return ["?"]

    def _obd(self, request: bytes) -> list[str]:
        extended = self.protocol not in (6, 8)
        can_id = self.header_29 if extended else self.header_11
        now = 0
        raw = self.bus.on_frame(CANFrame(can_id, bytes([len(request)]) + request, extended), now)
        raw += self.bus.tick(60_000)  # deterministic virtual time, no wall-clock sleep
        if not raw:
            return ["NO DATA"]
        replies: dict[int, list[CANFrame]] = {}
        for frame in raw:
            replies.setdefault(frame.can_id, []).append(frame)
        for rid, frames in list(replies.items()):
            first = frames[0]
            if not first.data or first.data[0] >> 4 != 1 or not self.flowcontrol:
                continue
            tester_id = (0x18DA0000 | ((rid & 255) << 8) | 0xF1) if extended else rid - 8
            following = self.bus.on_frame(CANFrame(tester_id, b"\x30\x00\x00", extended), 60_000)
            replies[rid].extend(following)
        lines = []
        for rid, frames in sorted(replies.items()):
            first = frames[0]
            if not first.data:
                continue
            if not self.autoformat:
                for f in frames:
                    label = f"{rid:08X}" if extended else f"{rid:03X}"
                    data = _print_hex(f.data, self.spaces)
                    lines.append((label + (" " if self.spaces else "") if self.headers else "") + data)
                continue
            pci = first.data[0] >> 4
            if pci == 0:
                n = first.data[0] & 0x0F
                pdu = first.data[1:1 + n]
            elif pci == 1 and self.flowcontrol:
                length = ((first.data[0] & 15) << 8) | first.data[1]
                pdu = (first.data[2:] + b"".join(f.data[1:] for f in frames[1:]))[:length]
                if len(pdu) != length:
                    continue
            else:
                continue
            if not pdu:
                continue
            label = (f"{rid:08X}" if extended else f"{rid:03X}") + (" " if self.spaces else "")
            lines.append((label if self.headers else "") + _print_hex(pdu, self.spaces))
        return lines or ["NO DATA"]
