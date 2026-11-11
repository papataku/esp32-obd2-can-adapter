"""Classic-CAN ECU bus simulator: deterministic and free of physical CAN APIs.

One virtual ECU generates positive or negative read-only Mode 01 / UDS22 PDUs.
The tester owns ISO-TP FC; this module never fabricates FC on behalf of it.
"""
from __future__ import annotations
from dataclasses import dataclass
from .model import ECUModel, ECU, Response


@dataclass(frozen=True)
class CANFrame:
    can_id: int
    data: bytes
    extended: bool
    def __post_init__(self):
        if type(self.can_id) is not int or not 0 <= self.can_id <= (
            0x1FFFFFFF if self.extended else 0x7FF
        ):
            raise ValueError("CAN arbitration ID out of range")
        if not isinstance(self.data, bytes) or len(self.data) > 8:
            raise ValueError("Classic CAN DLC exceeds 8")


@dataclass
class _Incoming:
    length: int
    buf: bytearray
    next_seq: int
    ecu: ECU


@dataclass
class _Outgoing:
    can_id: int
    extended: bool
    chunks: list[bytes]
    next_seq: int = 1
    wait_fc: bool = True
    remaining_block: int = 0
    stmin_ms: int = 0
    next_due_ms: int = 0


class ECUSimulator:
    def __init__(self, model: ECUModel):
        self.model = model
        self.attempts: dict[tuple[str, bytes], int] = {}
        self._scheduled: list[tuple[int, CANFrame, bytes]] = []
        self._outgoing: dict[tuple[bool, int], _Outgoing] = {}
        self._incoming: dict[tuple[bool, int], _Incoming] = {}

    def _eligible(self, frame: CANFrame):
        for ecu in self.model.ecus:
            if frame.extended:
                ecu_number = int(ecu.id[3:], 16)
                if frame.can_id not in (0x18DB33F1, 0x18DA0000 | (ecu_number << 8) | 0xF1):
                    continue
            else:
                ids = [r.response_id_11 for r in ecu.responses if r.response_id_11 is not None]
                if frame.can_id != 0x7DF and not any(frame.can_id == n - 8 for n in ids):
                    continue
                if not ids:
                    continue
            yield ecu

    @staticmethod
    def _response_id(ecu: ECU, request: bytes, extended: bool) -> int | None:
        for response in ecu.responses:
            if response.request == request:
                return response.response_id_29 if extended else response.response_id_11
        return None

    @staticmethod
    def _value_at(record: Response, now_ms: int) -> tuple[bytes | None, int]:
        """Sample-and-hold lookup against the caller's monotonic model clock.

        sequence: hold last sample after final timestamp.
        loop: wrap with the explicitly supplied period, including gap at end.
        fixed: preserve legacy payload and delay semantics.
        """
        if record.replay_mode == "fixed":
            return record.payload, record.delay_ms
        t = (now_ms % record.loop_period_ms
             if record.replay_mode == "loop" else now_ms)
        series = record.samples
        lo, hi = 0, len(series)
        while lo < hi:
            mid = (lo + hi) // 2
            if series[mid].at_ms <= t:
                lo = mid + 1
            else:
                hi = mid
        sample = series[lo - 1]  # schema requires first sample at 0
        delay = record.delay_ms if sample.delay_ms is None else sample.delay_ms
        return sample.payload, delay

    def _queue_pdu(self, ecu: ECU, request: bytes, extended: bool, now_ms: int):
        for record in ecu.responses:
            if record.request != request:
                continue
            key = (ecu.id, request)
            count = self.attempts.get(key, 0) + 1
            self.attempts[key] = count
            if record.drop_every and count % record.drop_every == 0:
                continue
            rid = record.response_id_29 if extended else record.response_id_11
            if rid is None:
                continue
            payload, delay_ms = self._value_at(record, now_ms)
            if payload is None:
                # A synthetic no_data window is indistinguishable on the wire
                # from an ECU with no response; callers decide their timeout.
                continue
            if len(payload) <= 7:
                frame = CANFrame(rid, bytes([len(payload)]) + payload, extended)
                self._scheduled.append((now_ms + delay_ms, frame, b""))
            else:
                length = len(payload)
                first = bytes([0x10 | ((length >> 8) & 15), length & 255]) + payload[:6]
                self._scheduled.append((now_ms + delay_ms,
                                        CANFrame(rid, first, extended), payload[6:]))

    def _deliver_request(self, frame: CANFrame, request: bytes, now_ms: int, ecu: ECU | None = None):
        if not ((len(request) == 2 and request[0] == 1) or
                (len(request) == 3 and request[0] == 0x22)):
            return
        for item in ((ecu,) if ecu else self._eligible(frame)):
            self._queue_pdu(item, request, frame.extended, now_ms)

    def on_frame(self, frame: CANFrame, now_ms: int) -> list[CANFrame]:
        if type(now_ms) is not int or now_ms < 0:
            raise ValueError("time must be monotonic nonnegative milliseconds")
        if not frame.data:
            return self.tick(now_ms)
        pci = frame.data[0] >> 4
        key = (frame.extended, frame.can_id)
        if pci == 3:
            # The tester must send FC to the matching ECU's *request* CAN ID.
            for transfer_key, tx in list(self._outgoing.items()):
                if transfer_key[0] != frame.extended:
                    continue
                expected = (0x18DA0000 | ((tx.can_id & 255) << 8) | 0xF1) if frame.extended else tx.can_id - 8
                if frame.can_id != expected:
                    continue
                if len(frame.data) < 3:
                    return self.tick(now_ms)
                status = frame.data[0] & 15
                if status == 2:
                    del self._outgoing[transfer_key]
                elif status == 0:
                    tx.wait_fc = False
                    tx.remaining_block = frame.data[1] or 65535
                    stmin = frame.data[2]
                    if stmin <= 0x7F:
                        tx.stmin_ms = stmin
                    elif 0xF1 <= stmin <= 0xF9:
                        tx.stmin_ms = 1  # conservative 1ms scheduling granularity
                    else:
                        del self._outgoing[transfer_key]
                        return self.tick(now_ms)
                    tx.next_due_ms = now_ms + tx.stmin_ms
                return self.tick(now_ms)
        elif pci == 0:
            length = frame.data[0] & 15
            if length and length <= len(frame.data) - 1:
                self._deliver_request(frame, frame.data[1:1 + length], now_ms)
        elif pci == 1 and len(frame.data) >= 3:
            length = ((frame.data[0] & 15) << 8) | frame.data[1]
            if length > 7 and length <= 4095:
                # Long requests are allowed only to a unique *physical* ECU.
                targets = list(self._eligible(frame))
                if len(targets) == 1 and frame.can_id not in (0x7DF, 0x18DB33F1):
                    self._incoming[key] = _Incoming(length, bytearray(frame.data[2:]), 1, targets[0])
                    rec = targets[0].responses[0] if targets[0].responses else None
                    if rec:
                        rid = rec.response_id_29 if frame.extended else rec.response_id_11
                        if rid is not None:
                            self._scheduled.append((now_ms, CANFrame(rid, b"\x30\x00\x00", frame.extended), b""))
        elif pci == 2 and key in self._incoming:
            inc = self._incoming[key]
            seq = frame.data[0] & 15
            if seq != inc.next_seq or len(frame.data) < 2:
                del self._incoming[key]
            else:
                inc.buf.extend(frame.data[1:])
                inc.next_seq = (inc.next_seq + 1) & 15
                if len(inc.buf) >= inc.length:
                    del self._incoming[key]
                    self._deliver_request(frame, bytes(inc.buf[:inc.length]), now_ms, inc.ecu)
        return self.tick(now_ms)

    def tick(self, now_ms: int) -> list[CANFrame]:
        if type(now_ms) is not int or now_ms < 0:
            raise ValueError("invalid monotonic time")
        output = []
        later = []
        for due, first, remaining in self._scheduled:
            if due > now_ms:
                later.append((due, first, remaining))
                continue
            output.append(first)
            if remaining:
                chunks = [remaining[i:i + 7] for i in range(0, len(remaining), 7)]
                key = (first.extended, first.can_id)
                self._outgoing[key] = _Outgoing(first.can_id, first.extended, chunks)
        self._scheduled = later
        for key, tx in list(self._outgoing.items()):
            emitted = 0
            while not tx.wait_fc and tx.chunks and now_ms >= tx.next_due_ms and emitted < 32:
                chunk = tx.chunks.pop(0)
                output.append(CANFrame(tx.can_id, bytes([0x20 | tx.next_seq]) + chunk, tx.extended))
                tx.next_seq = (tx.next_seq + 1) & 15
                tx.remaining_block -= 1
                emitted += 1
                tx.next_due_ms += tx.stmin_ms
                if tx.remaining_block == 0 and tx.chunks:
                    tx.wait_fc = True
            if not tx.chunks:
                del self._outgoing[key]
        return output
