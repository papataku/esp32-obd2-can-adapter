# Phase 3A — ELM327/KW905 Command Surface

Phase 3A adds the ELM command surface used by the current Honda Analyzer **without enabling vehicle CAN TX**.

## Why

The current KW905 path is performance-limited, but it is also the existing compatibility reference. Before replacing it, M5CAN should accept the same initialization/header/timing commands so the exact same Analyzer workflow can be A/B tested.

The actual Analyzer uses ELM protocol 7:

```text
ISO 15765-4
29-bit CAN ID
500 kbit/s
```

This supersedes the earlier 11-bit-first assumption for the M5CAN compatibility path.

## Auto entry

M5CAN still boots in the Phase 2 text monitor.

The first AT-style command, normally:

```text
ATZ
```

automatically enters ELM compatibility mode.

Direct read-like hex commands such as `010C` or `222012` also enter ELM mode, but in Phase 3A they remain TX locked.

While ELM mode is active:

- Phase 2 RAW text streaming is disabled
- periodic `#STATS` text is suppressed
- commands are CR terminated
- prompt is `>`
- echo follows ATE0/1
- linefeed follows ATL0/1
- spaces state follows ATS0/1
- headers state follows ATH0/1

`ATM5TEXT` returns to the M5CAN debug text mode.

## Implemented AT commands

Current Honda Analyzer initialization/meta/header/timing commands:

```text
ATZ
ATE0 / ATE1
ATL0 / ATL1
ATS0 / ATS1
ATH0 / ATH1
ATAL
ATCAF0 / ATCAF1
ATCFC0 / ATCFC1
ATSP7
ATI
ATDP
ATDPN
AT@1
ATCPxx
ATSHxxxxxx
ATAT0 / ATAT1 / ATAT2
ATSTxx
```

Unsupported AT commands return `?`.

M5CAN identifies itself honestly:

```text
M5CAN v0.3 ELM-CAN compatible
```

It does not claim to be a genuine ELM327.

## Classic 29-bit header behavior

To match the real KW905 clone behavior:

```text
ATCP18
ATSHDB33F1
```

selects effective CAN ID:

```text
18DB33F1
```

and:

```text
ATSHDBEFF1
```

selects:

```text
18DBEFF1
```

An 8-digit `ATSH18DB33F1` is deliberately rejected with `?`, matching the compatibility behavior already encoded in Honda Analyzer tests.

## Vehicle-request parser

Phase 3A recognizes only read forms needed by the Analyzer:

```text
01xx
09xx
22xxxx
```

Examples:

```text
0100
010C
010D
0105
015B
019A
222012
```

But no CAN TX exists in Phase 3A. Such requests return:

```text
M5CAN TX LOCKED
>
```

Dangerous/write/control services are not accepted by the parser.

Phase 3B adds the single-owner TWAI state machine and explicit short TX lease. Vehicle TX remains blocked by the hardware gates in Issues #1 and #3.
