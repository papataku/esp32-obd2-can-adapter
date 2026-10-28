# V2-2 29bit M5Dial + SmartRing vehicle trial — OWNER ONLY

**STOP:** Draft build; independent PM/Codex safety review and successful CI + host/virtual tests are mandatory before owner physical work. No agent flashing or vehicle CAN TX is permitted.

## Full backup / verified rollback
Identify both boards, flash capacity, installed app slot, partition table, otadata, build SHA and `ATI`. Follow the previously approved V2-1b backup and app-only write procedure. Read the complete flash of each device into *local protected storage*, independently hash and verify readback and retain original firmware/partition offsets. Do not publish BLE MAC, VIN, locations, bonds or NVS images. Never erase flash, change partitions, rewrite bootloader or use a generic PlatformIO upload. The operator must confirm installed OTA versus single-app partition is identical to the target build. For rollback, STOP CAN, reflash original app bytes to the exact prior app slot, hash-verify and reboot; escalate full-flash restore separately if layout differs.

## Example offline operator commands (NOT executed by this PR)
Only after independent safety sign-off and verifying the *installed* table and active boot slot, repeat for EACH board separately. Define PORT, FLASH_SIZE, APP_OFFSET, APP_LENGTH and FIRMWARE_BIN from the verified device inventory; never use a guessed offset or any `erase_flash` command.

```sh
# Local protected workstation, separately for M5Dial and SmartRing:
python3 -m esptool --chip esp32s3 --port "$PORT" flash_id
python3 -m esptool --chip esp32s3 --port "$PORT" read_flash 0 "$FLASH_SIZE" full-before.bin
python3 -m esptool --chip esp32s3 --port "$PORT" read_flash "$APP_OFFSET" "$APP_LENGTH" app-before.bin
shasum -a 256 full-before.bin app-before.bin "$FIRMWARE_BIN"
# Confirm the app-only target size fits the currently ACTIVE app partition.
test "$(wc -c < "$FIRMWARE_BIN")" -le "$APP_LENGTH" || exit 1
# Owner-only, only after approval:
python3 -m esptool --chip esp32s3 --port "$PORT" write_flash "$APP_OFFSET" "$FIRMWARE_BIN"
python3 -m esptool --chip esp32s3 --port "$PORT" verify_flash "$APP_OFFSET" "$FIRMWARE_BIN"
# Restore the original app using the saved image, NOT the newly built binary:
python3 -m esptool --chip esp32s3 --port "$PORT" write_flash "$APP_OFFSET" app-before.bin
python3 -m esptool --chip esp32s3 --port "$PORT" verify_flash "$APP_OFFSET" app-before.bin
```
Do not run the example as one shell script: the final two commands are a **rollback branch**, not a continuation after flashing. Do not set or share PORT, saved flash, or any identity in GitHub. If otadata or active slot changed, STOP rather than writing the wrong app slot.

## Vehicle steps (no interaction while driving)
1. Park, parking brake P, IG ON. In listen-only, record whether the owner's ECU actually uses 29bit/500kbps Mode01, identify physical reply 18DAF1xx from the bounded functional 01 0C 0D probe. Reject 11bit 7E8–7EF, a changed ECU responder set, multiple viable ECU responders, NO DATA or incomplete RAW evidence. Do not infer vehicle identity from NVS hint.
2. Connect already-registered, encrypted SmartRing. Build matching explicit 5Hz images. The USB owner locally issues `SIGV2 CAN ARM` only after confirming the validated pairing and vehicle connection. Record pinned ECU, bounded discovery attempts, SourceGeneration, physical response ID and CAN TX count.
3. Stationary IG ON then idle: require each PID 0D/0C/05/5B/9A Valid or explicit Invalid, actual observed 5Hz rates, no false zero and no stale Fresh; monitor `#SIGNALV2_LOAD`, `#SIGNALV2_CAN`, SmartRing invalid/rejected counters. Inspect real external analyzer if available.
4. With a second person recording and driver not touching the equipment, only low-speed driving after prior acceptance. Then, only if stable, **explicitly change both** images to opt-in 100ms/10Hz and repeat from stationary. Record actual Hz per PID (requested Hz is not sufficient evidence).
5. Fault injection (wrong ECU, ISO-TP corruption, timeout, disconnection, 11bit reply and load spikes) belongs on the host/virtual simulator, not the real vehicle.

## Abort criteria and evidence
Immediately STOP on any car warning lamp, bus-off, TWAI error/overrun, request-load >5% rolling 1s or >10% rolling 100ms, unverified/changed ECU, 11bit diagnostic response, absent/Invalid speed, or failed recovery. `SIGV2 CAN STOP`, park safely, disconnect trial equipment, rollback original firmware, document reason without identifiers.

Preserve both FW SHAs, `ATI`, exact env and partition hashes locally, GET/quality per PID, requested and actual Hz, `#SIGNALV2_LOAD` worst-case bits, externally measured load if possible, `#SIGNALV2_CAN` Invalid/rejection counts, Ring quality/gauge screenshots and driver warning-light status. Before sharing evidence, remove personal or vehicle identifiers.
