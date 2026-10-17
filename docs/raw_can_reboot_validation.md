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


## Follow-up: SmartRing physical SD and adapter recovery

Firmware `9c5c47a7ce77dfd68da9c9e6a53082d0bc26966a` was paired with
meter `9860d73f22f54d67afb1c33324369b06908cc25d`. The user accepted a
20-minute soak; positive Recording status samples span 1,321.870 seconds.
Two BLE meters stayed connected, SmartRing SDMMC CSV reached 8,976,676 bytes,
and normal STOP imported the actual retained adapter blocks via BLE and
verified the complete physical SD file readback:

```text
M5CAN RAW SD VERIFIED path=/logs/m5can_19ACEE8D.raw blocks=3 bytes=176
#CANLOG,ready=1,recording=0,stopping=0,fault=0,session=430763661,queued=0,drop=0,blocks=3,bytes=140,store_err=0
```

After explicitly disconnecting both peers, USB `CANLOG INDEX` / READ 0-2
recovered the same retained session without erasure. The decoder passed all
MCAN CRCs, 5 TX_SUBMITTED events, zero sequence gaps and the END marker.
DRIVELOG and BLELOG exports also reached END. Boot 122 shows group request 1,
individual requests 4, group success 0, fallback 1, CAN RX 0, TX 5/5,
no-data/timeouts 5 from the intentional 1.2-second DEMO OFF bench stimulus.
This demonstrates the missing-ECU fallback, not successful ECU grouping or
wire delivery. Steady-state meter PID errors did not increase thereafter.

BLELOG's bounded retained history has 23 total / 12 kept / 11 evicted,
queue drops 0 and sequence gaps 0; it is not a full event archive. RAW drops,
store errors, notify failures/retries and RX overflow were 0. The diagnostic
exports require idle/no BLE peers; that guard was verified. Two RPM meters
were restored connected, local DEMO OFF, adapter DEMO AUTO, RAW stopped.

Full report, exact hashes, sampled memory/stack/polling/display measurements,
CSV and remaining gates:
[TacoTacoMeter hardware report](https://github.com/papataku/TacoTacoMeter/blob/feature/t4s3-sd-default-log-export/docs/validation/2026-10-17/smartring-sd-bench.md).
T4-S3 SD initialization, real CAN RX/high load, power interruption, export
security and repeated-session/reclaim lifecycle remain open. No one-hour or
visual/touch acceptance is claimed. Draft remains appropriate.
