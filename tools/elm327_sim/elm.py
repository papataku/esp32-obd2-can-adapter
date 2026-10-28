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
        self.rx_filter = None
        self.detected_protocol = None
        self.last_response_delay_ms = None
        self.max_response_bytes = 8192
        self._next_fault = None
        self.virtual_voltage = "12.6V"  # synthetic voltage, not hardware telemetry
        self.virtual_now_ms = 0
        self.baud_divisor = None
        self.baud_pending = None
        self.baud_timeout_ms = 75
        self.baud_deadline_ms = None

    def inject_fault_once(self, code: str):
        """Synthetic fault, not a public AT command or physical bus operation."""
        if code not in ("CAN ERROR", "BUFFER FULL"):
            raise ValueError("invalid fault injection")
        self._next_fault = code

    def advance_virtual_time(self, elapsed_ms: int):
        if type(elapsed_ms) is not int or elapsed_ms < 0:
            raise ValueError("time must be monotonic")
        self.virtual_now_ms += elapsed_ms
        if self.baud_pending is not None and self.virtual_now_ms > self.baud_deadline_ms:
            self.baud_pending = None
            self.baud_deadline_ms = None

    def baud_test_identity(self):
        """Synthetic BRD test banner. Not an actual UART speed change."""
        if self.baud_pending is None:
            raise ValueError("no pending BRD")
        return b"ELM327 SIMULATOR v0.1\r"

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
        if self.baud_pending is not None:
            # ELM327DSK pp.51-52: BRD waits for the host's CR at the
            # candidate rate. This models the handshake, never UART hardware.
            if not original:
                self.baud_divisor = self.baud_pending
                self.baud_pending = None
                self.baud_deadline_ms = None
                return b"\r>"
            self.baud_pending = None
            self.baud_deadline_ms = None
            return b"?\r>"
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
        if self.baud_pending is not None and original.startswith("ATBRD"):
            # Initial OK, then rate change and test banner; NO final prompt.
            return result + b"OK\r"
        if self.monitoring and original == "ATMA":
            return result  # monitor stream is not completed by a prompt
        result += b"".join(self._format_line(line) for line in lines if line is not None)
        return result + b">"

    def _execute(self, cmd: str) -> list[str]:
        if cmd in ("ATZ", "ATWS"):
            baud = self.baud_divisor
            self.reset()
            if cmd == "ATWS":
                self.baud_divisor = baud  # warm start preserves negotiated rate
            return ["ELM327 SIMULATOR v0.1"]
        if cmd == "ATD":
            baud = self.baud_divisor
            self.reset()
            self.baud_divisor = baud  # D restores settings, not serial hardware rate
            return ["OK"]
        if cmd == "ATI":
            return ["ELM327 SIMULATOR v0.1"]
        if cmd == "ATRV":
            return [self.virtual_voltage]
        if cmd.startswith("ATBRD") and len(cmd) == 7:
            try:
                divisor = int(cmd[5:], 16)
            except ValueError:
                return ["?"]
            if not 1 <= divisor <= 255:
                return ["?"]
            self.baud_pending = divisor
            self.baud_deadline_ms = self.virtual_now_ms + self.baud_timeout_ms
            return ["OK"]
        if cmd.startswith("ATBRT") and len(cmd) == 7:
            try:
                h = int(cmd[5:], 16)
            except ValueError:
                return ["?"]
            self.baud_timeout_ms = 5 * (h if h else 256)
            return ["OK"]
        if cmd == "AT@1":
            return ["OBDII to RS232 Interpreter"]
        if cmd == "ATDP":
            selected = self.detected_protocol if self.protocol == 0 else self.protocol
            if selected is None:
                return ["AUTO"]
            size = 11 if selected in (6, 8) else 29
            rate = 500 if selected in (6, 7) else 250
            return [("AUTO, " if self.protocol == 0 else "") +
                    f"ISO 15765-4 (CAN {size}/{rate})"]
        if cmd == "ATDPN":
            if self.protocol == 0:
                return [("A" + str(self.detected_protocol)) if self.detected_protocol else "0"]
            return [str(self.protocol)]
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
            self.detected_protocol = None
            self.last_response_delay_ms = None
            return ["OK"]
        if cmd == "ATCRA":
            self.rx_filter = None
            return ["OK"]
        if cmd.startswith("ATCRA"):
            import re
            pattern = cmd[5:]
            if (len(pattern) not in (3, 8) or
                    not re.fullmatch(r"[0-9A-FX]+", pattern) or
                    (len(pattern) == 3 and pattern[0] not in "01234567X")):
                return ["?"]
            self.rx_filter = pattern
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
            # ELM327DSK protocol close; configured ATSP selection persists.
            self.detected_protocol = None
            self.last_response_delay_ms = None
            self.bus._scheduled.clear()
            self.bus._outgoing.clear()
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
        if self._next_fault is not None:
            fault, self._next_fault = self._next_fault, None
            return [fault]
        if self.protocol == 0 and self.detected_protocol is None:
            # CAN-only virtual auto-search (NOT nine-protocol vehicle search).
            candidates = [r for ecu in self.bus.model.ecus for r in ecu.responses
                          if r.request == request]
            if any(r.response_id_29 is not None for r in candidates):
                self.detected_protocol = 7
            elif any(r.response_id_11 is not None for r in candidates):
                self.detected_protocol = 6
            else:
                return ["UNABLE TO CONNECT"]
        chosen = self.detected_protocol if self.protocol == 0 else self.protocol
        extended = chosen not in (6, 8)
        can_id = self.header_29 if extended else self.header_11
        # Virtual clock in milliseconds. ST units are 4ms (ELM327DSK p.53).
        base_deadline = (self.timeout_4ms or 256) * 4
        deadline = base_deadline
        if self.adaptive and self.last_response_delay_ms is not None:
            # Deterministic approximation ONLY of ATAT; not proprietary algorithm.
            slack = 8 if self.adaptive == 1 else 4
            deadline = min(deadline, max(4, self.last_response_delay_ms + slack))
        raw = self.bus.on_frame(
            CANFrame(can_id, bytes([len(request)]) + request, extended), 0)
        due_times = sorted({due for due, _frame, _rest in self.bus._scheduled
                            if due <= deadline})
        for due in due_times:
            raw.extend(self.bus.tick(due))
        self.bus._scheduled.clear()  # discard late transaction data, no stale bleed
        if not raw:
            self.bus._outgoing.clear()
            return ["NO DATA"]
        replies: dict[int, list[CANFrame]] = {}
        for frame in raw:
            if self.rx_filter is not None:
                label = f"{frame.can_id:08X}" if extended else f"{frame.can_id:03X}"
                if len(label) != len(self.rx_filter) or any(
                    want != "X" and want != found
                    for want, found in zip(self.rx_filter, label)
                ):
                    continue
            replies.setdefault(frame.can_id, []).append(frame)
        if not replies:
            self.bus._outgoing.clear()
            return ["NO DATA"]
        # Adaptive estimate must use *accepted* responder timing; filtered
        # ECUs cannot extend the learned reply window (DSK p.53).
        observed_delays = [
            record.delay_ms
            for ecu in self.bus.model.ecus for record in ecu.responses
            if record.request == request and (
                (record.response_id_29 if extended else record.response_id_11)
                in replies
            )
        ]
        self.last_response_delay_ms = max(observed_delays) if observed_delays else 0
        for rid, frames in list(replies.items()):
            first = frames[0]
            if not first.data or first.data[0] >> 4 != 1 or not self.flowcontrol:
                continue
            tester_id = (0x18DA0000 | ((rid & 255) << 8) | 0xF1) if extended else rid - 8
            following = self.bus.on_frame(CANFrame(tester_id, b"\x30\x00\x00", extended), max(due_times) if due_times else 0)
            replies[rid].extend(following)
        lines = []
        for rid, frames in replies.items():
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
            if pci == 1:
                if self.headers:
                    for f in frames:
                        lines.append(label + _print_hex(f.data, self.spaces))
                else:
                    length_text = f"{len(pdu):03X}"
                    lines.append(length_text)
                    blocks = [pdu[:6]] + [pdu[i:i + 7] for i in range(6, len(pdu), 7)]
                    for index, block in enumerate(blocks):
                        lines.append(f"{index:X}: " + _print_hex(block, self.spaces))
            else:
                lines.append((label if self.headers else "") + _print_hex(pdu, self.spaces))
        self.bus._outgoing.clear()
        if sum(len(line.encode("ascii")) + 2 for line in lines) > self.max_response_bytes:
            return ["BUFFER FULL"]
        return lines or ["NO DATA"]
