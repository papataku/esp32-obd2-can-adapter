# RAW journal reboot metadata — physical bench findings

At PR #32 baseline `5992fbbbd2766167df7c395bf9f9671ecf41181b`, the approved
single-factory layout was installed on the disconnected M5Dial USB bench.
Bootloader, NVS, retained evidence and coredump region hashes matched across
migration. Boot advertised RAW `ready=1` with recording off.

## Actual observations

An explicit USB recording, with two BLE meters and a 1.2-second DEMO OFF
interval, retained seven real `twai_transmit` software submission observations.
No vehicle was connected; **no ECU RX and no successful physical delivery were
established**. The first payload was grouped Mode 01 RPM+speed
`03 01 0C 0D 00 00 00 00`, followed by six individual read submissions after
the expected no-response fallback. The seven observations were recovered by
`recover_raw_can_log.py` from USB block readout: two committed blocks including
END, CRC valid, zero observed sequence gaps and zero reported capture drops.

The same bytes and END marker survived a USB-triggered reset. However,
`CANLOG STATUS` reported `session=0` while `CANLOG INDEX` correctly reported
`session=32011917`. The service restored state and block counts from the
journal but omitted its session ID. Its drop API likewise read only the fresh
RAM capture counter, losing a previously persisted nonzero drop count.

`begin()` now publishes the verified journal session and saved drop baseline.
The drop API reports the greater of that baseline and the live counter. A
newly accepted capture resets the baseline. Startup still never resumes
recording, claims a BLE owner, erases retained bytes or grants a CAN TX lease.

The host regression exercises production service initialization with NOR-flash
semantics for complete and incomplete sessions, nonzero saved drops, refusal
to overwrite retained data and unreadable storage. It tests public status
behavior rather than only the journal parser.

## Acceptance limits

The T4-S3 PR #119 recording trial was blocked by a separate SD logger panic.
That firmware now isolates its SD controller and increases log task stack.
The available card subsequently failed ACMD41 initialization even with LCD
absent; comparison with another card is requested. Physical BLE RAW-to-SD
completion and a one-hour recording soak remain pending, as do real CAN RX,
electrical power loss, secure export and subsequent-session reclaim.

USB reset retention is not an electrical power-loss test. Software
TX_SUBMITTED is not a wire-level timestamp or an ECU acknowledgement.
