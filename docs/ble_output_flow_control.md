# Bounded dual-meter BLE output

Two peers share NimBLE-Arduino's fixed packet pools. In the old path each complete ELM response was synchronously submitted in20-byte chunks. A zero return from `ble_gattc_notify_custom()` also means accepted into `bhc_tx_q`; it does not prove radio delivery. A slow link therefore allowed the application producer to fill the shared pool. A later `ble_hs_mbuf_from_flat()` failed and the100ms whole-write deadline either cut the link locally or delayed application servicing until a meter gave up.

On the physical diagnostic run with firmware `ef920a1`, MSYS free blocks reached0 and thousands of allocation failures occurred; notify API failure count stayed0. Both meters and connection-order effects are assessed using archived serial traces, not general heap/free-memory inference. Detailed measured comparisons are added to the acceptance report after the runs finish.

The corrected dual application uses `queueTo()` and `pollTx()`:

- Each connection has a4096-byte fixed RAM FIFO. Acceptance into this FIFO is separate from successful GATT submission counters. A whole write is accepted or rejected; no partial enqueue is reported as success.
- The worker alternates which peer is serviced first and tries at most three notifications per peer per poll. It does not sleep or spin retrying a blocked peer.
- Under the NimBLE host mutex, the transport checks the connection's controller outstanding packet count and host queue. No additional notification is submitted when2 packets are already host-queued or at least8 controller+host packets belong to the peer. At least6 free MSYS blocks are required before allocating; this leaves4 blocks when a notification retains two. The small bounded host pipeline is essential: forbidding all host queueing reduced high-rate throughput because the application worker could not refill controller slots within a radio event. This read-only use of NimBLE private connection state is isolated in `notifyCredit()` and the dependency is pinned to the verified version1.4.3.
- Before advancing a batch or submitting another CAN request, the producer waits until at most1024 bytes remain in that peer FIFO. This reserves space for one maximum48-frame RAW result and the512-byte session staging buffer. Cached/DEMO batches resume after the FIFO drains; their final prompt stays ordered.
- Transient allocation/API backpressure retains the same bytes and resumes in order. Only successful API submission advances the FIFO. No extra free is added: NimBLE consumes the supplied mbuf on both success and failure.
- A reconnect clears pending output and receives a new generation. Completions from an earlier generation cannot advance or overwrite the new FIFO.
- FIFO overflow, a permanent API error, or5 seconds without per-peer progress explicitly records failure and requests disconnection only for that peer. The aborted peer is quarantined immediately while GAP delivery is pending, preventing a trailing prompt from escaping after a rejected response. Deadline subtraction is unsigned to handle clock wrap.

`#BLE_MEMORY` distinguishes allocation/API failures and reports minimum free blocks and last error identity/time. `#BLE_FLOW` reports credit waits, RAM FIFO peak, observed controller+host backlog peak and separate host-queue depth, overflow and stall counts. A credit wait is expected flow control, not a lost response or notify failure. `BLE_PEER` byte/notification counters still count successful GATT submissions, not FIFO acceptance or acknowledged radio bytes.

Stable BLELOG error code104 additionally uses detail0x10000 for RAM output overflow and0x20000 for a5s no-progress timeout. Other values remain the raw NimBLE error code. Code103 retains retry-count detail semantics. No persistent record layout/catalog size changes are introduced.

The old synchronous producer exists only in `M5CAN_SYNC_NOTIFY_DIAGNOSTIC` builds. `m5dial_memory_diagnostic_baseline` enables it for cause-isolation with physical CAN TX compiled out. It is absent from normal dual builds. The existing single-client firmware is unchanged.

Host tests exercise a blocked peer while the other continues, recovery after200ms, exact byte ordering/FIFO wrap, allocation races, shared-pool reserve, stale generations, atomic overflow/quarantine and a genuine stuck link. Host results do not establish physical CAN, LCD/touch or radio acceptance. A matched physical high-rate comparison and a2h two-meter run are required before declaring the disconnect fixed.
