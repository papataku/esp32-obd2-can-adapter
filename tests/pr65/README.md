# PR #65 independent safety review regressions

Ported from the independent review's `reviewer-evidence/{monitor,owner,policy,profile-odr,profile-reader}.cpp` at reviewed head `10a506d7f1cd9f9625267125b6dbe972dc525ffd`. The assertions now require the safe behavior rather than asserting the bug. `monitor.cpp` links the production monitor, transaction, policy and assembler; only the TWAI boundary is scheduled by the fixture. `owner.cpp` replaces the monitor API to inject queued completion timing, admission failure and health counters. The two-translation-unit profile test forces real array ODR use at `-O0` in C++11 and C++17.

Run `bash tests/run_pr65_regressions.sh` (also included in `tests/run_all.sh`). Each finding runs in a separate process; one failure cannot hide later findings. ASan/UBSan cover the monitor and owner fixtures. The suite also preserves the review's budget, no-FC, empty-profile, epoch-isolation, negative-response and regular ISO-TP controls, plus default/V1/V2 policy configurations.

| Finding / test selector | Regression |
| --- | --- |
| `monitor M1` | Wrong ECU, zero-length SF, unrequested PID and unsupported PCI before a good reply cannot pin. |
| `monitor M2` | Candidate reply at +130 ms is charged, latches fault, and prevents subsequent TX; queued late RX survives driver reinstall. |
| `monitor M3` | Both split-PID orders, including a second reply at +110 ms, produce two RAW-backed responses and a pin. |
| `owner M4` | Good same-epoch completion delivered at the absolute 3-second deadline cannot pin. |
| `owner M5` | Failed submit stops after one attempt; failed lease stops with zero submissions. |
| `monitor M6`, `owner M6` | Lower 150 ms probe spacing denied; 10 Hz owner waits 200 ms for first regular group. |
| `owner M7`, `monitor M7` | RX missed/overrun or unavailable driver status prevents pin and further TX. |
| `monitor m1`, `policy` | V2 functional policy and TX denied, with both empty and RP8 profiles; V1/default policy preserved. |
| `profile-odr` (`c++11`, `c++17`) | Taking the candidate-array address across translation units links and runs in both standards. |

Before the production fixes, every original finding failed its safe-behavior assertion (M6 owner in 10 Hz), and the C++11 profile test failed to link with undefined `DiscoveryProfile::kCandidates`. This was verified with the original reproductions ported first, before changing production sources. Later additions cover lease failure, both split-PID orders, lower driver status/loss injection, and upper RAW-offset validation.

These are host software tests and firmware build checks. No HIL, firmware upload or vehicle operation is performed, and no physical ECU response ceiling is established here.
