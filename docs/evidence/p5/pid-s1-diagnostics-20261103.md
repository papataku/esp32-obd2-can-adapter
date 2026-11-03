# PID expansion S1 — read-only diagnostics

[計画](https://github.com/papataku/tacotaco-meter-ios/blob/4be832d2d0aac9bc4b650a63145c3b53a7f4efe7/docs/protocol/pid-expansion-plan.md) §10 item 1。固定adapter baseは `3e43dfe440d800b1ec82c17b6749e6e1ca322314`。

PM Claude 2026-11-03の決定により、adapter内のP3/P5 scopeだけに、以下の完全一致allowlistを追加する。iOS Public/Personal、Golden、liveAllowed、既存legacy-equivalence試験には変更しない。

- 診断ファイル: `include/signal_v2_pid_diagnostics.h`。
- 製品側追加symbol: `signal_v2::PidDiagnostics`（Count、pid、active_ms、last、due、epoch、active、pending、essential、kPeriodFloorMs、add、observe、admit、finish、cancel、shedAux、deny）、`CanOwner::pid_diagnostics_`、`CanOwner::pidDiagnostics`。
- 専用検査: `tools/test_pid_s1_scope.py`、`tools/test_pid_s1.py`、`tools/test_pid_s1_load.py`、`tests/pid_s1.cpp`。run_all末尾の2試験だけを追加する。

P3/P5では診断の完全一致挿入箇所だけを除去して元の固定base比較を実行する。P5のdiscovery検査は除去前のpreprocessor出力を使う。S1専用scopeはownerとrun_allを固定baseへ完全復元できること、診断ヘッダにCAN/lease/送信APIへの依存がないこと、要求長mutationが復元比較を通らないことを検査する。200 bits/frame、5000 bits/100ms、25000 bits/1s、二段gate、STAGE_GATED、要求集合、owner_live 5Hzと操作・fault規則は変更しない。

## カウンタの意味とread-out

`CanOwner::pidDiagnostics() const` はowner taskから読めるconst参照。ホストfixtureも同じAPIを読む。周期ログ、wire、BLE、CLIコマンド、予約設計は追加しない。デバイス向け新しい表示・USB公開は今回の受入に含めない。

順序は0D/0C/05/5B/9A。各PIDの累積値:

- demand: armedかつ非STOPで、既存merge後の最短周期（5Hz下限、既存10Hz labだけ100ms）ごとに全必須PIDの需要を数える。busy、discovery、予約拒否を分母から除外しない。遅れたtickはdueからまとめて数える。新しい需要・epochの最初を1件とする。STAGE_GATEDの観察はLIVE需要に数えない。
- admitted: probeを除くsubmit成功。TX成功数ではない。
- completed: 同epochの検証済みgroupを採用する直前。essentialなら0C/0Dだけ。NoData、invalid、旧epoch、probe、Snapshot、read-outで増えない。
- shed: fullからessentialへの選択時、補助3 PIDだけ。essential予約も拒否された場合にも選択されたshedを残す。
- denied: essential予算拒否では0C/0D、lease/submit拒否では選択された要求集合。失敗したprobeは計器拒否へ加算しない。
- failed: admittedした計器要求の検証失敗。cancelled: 未完了の計器要求をSTOP/epoch変更で破棄。両者はcompletedへ昇格しない。

全数値は64bit飽和加算。`active_ms`はtickで観測した有効需要区間の累積時間で、STOPで閉じる。窓の差分からPIDごとの需要Hz・完成Hz（差分completed×1000/差分active_ms）、完成/需要比、shed件数を計算する。0ms/0需要の窓は率未定義。初回需要はdiscoveryを含むため、完成/需要比を成功応答だけの成功率と呼ばない。需要集合/周期が変わったtickで現在のmerge周期を使い、tick間で観測できない購読の変化は推定しない。

カウンタは診断だけで完結し、戻り値をschedule/gate/予算/TXに使わない。既存memberのoffsetを保つためstorageをクラス末尾に追加する。counter全項目とactive_msを各tick前にUINT64_MAXへ強制しても、既存ownerシナリオのTX/RX traceはbase・通常版と一致しなければならない。

## 再現する検査

`python3 tools/test_pid_s1.py`（run_all末尾で実行、固定baseはshallow checkoutでも `git cat-file -e BASE^{commit} || git fetch --no-tags origin BASE` で取得）:

1. `tests/pid_s1.cpp` の独立期待値を、RP8既定構成と owner_live env の実効フラグの2構成で実行する。対象: 120ms遅延とbusy中の需要、rolling予約の219/220msと1119/1120ms境界、FC（FF+2CF RAW・FC 1回でもadmit 1/complete 1、ownerは追加TXしない）、all-RX（無関係RX 10万件でもdemand/completedは不変）、overrun、uncertain admission（submit失敗はSTOP、再試行TXなし）、STOP後100 tick（5秒）TX=0かつdemand/active_ms不変、同時需要（周期の異なる5 PIDを最短周期1本へmerge）、invalid応答、飽和加算。
2. 一時コピーだけで18 mutationを入れ、すべて実行時assertionで検出（compile失敗は検出に数えない）。ヘッダ7件（completion削除、essentialで全PID計上、shed削除、busy需要の過小計数、cancel削除、admission後だけ需要計数、飽和wrap）、ownerフック7件（shedAux/finish/admit/STOP cancel/2種のdeny削除、STOP後の需要計数）、owner挙動4件（常時shed、shedせずSTOP、uncertain admissionの再試行、STOP後TX）。
3. 既存ownerシナリオ11構成（owner_live実効フラグ、serial-HIL B-01、PR65 cadence 5Hz/10Hz lab、PR65 owner empty/RP8/10Hz lab、bounded discovery、stage S1 通常/STAGE_GATED、review owner）で、base `3e43dfe`・head・counter全項目UINT64_MAX強制・counter 0+内部状態(pending/essential/active/due/last/epoch)攪乱の4版の tick/TX/RX trace が完全一致。正の対照として deadline+1ms のTX変異はtraceを変える。
4. 既存production monitorのM1/M2/M3/M6/M7/m1/controls/R1/R2/R3で、driver境界の全TX（FC 12件を含む）と全RXがbase・head・極端counter版で一致。

`python3 tools/test_pid_s1_scope.py`: ownerとrun_allから8箇所の完全一致フックだけを除くと固定baseとbyte一致、診断ヘッダはCAN/lease/送信API・`this`・friendを含まず、struct/メソッド名が列挙どおり、内容はSHA-256で固定。P3/P5 scopeは同じフック一覧を使い、追加ファイル5件と記号名をリテラルで列挙（2026-11-03 PM決定を引用）。それ以外の判定は変更しない。

全suite: Python 3.11 + Apple clang と `docker run --rm -v "$PWD:/work" -w /work python:3.12 bash -c './tests/run_all.sh'`（GCC 14.2）。両方exit 0、warning 0。全envは platformio.ini の全 `env:` を列挙して `pio run -e` する。

## owner_live LOAD差分の説明（base 3e43dfe → S1 code commit 7f176e7）

`tools/test_pid_s1_load.py` で、同じtoolchainの通常buildと `-Wl,--emit-relocs` build（LOAD byteが通常buildと完全一致することを先に確認）を比べる。関数ごとにリンカ再配置箇所をmaskし、分岐/call先はsymbol+offset、`l32r` は読み込むliteralの値（文字列は内容）で比較する。

| LOAD | base | head | 差 | 説明 |
|---|---:|---:|---:|---|
| .iram0.vectors | 1027 | 1027 | 0 | 一致 |
| .iram0.text | 80195 | 80195 | 0 | 移動したflash/bss番地を指す語だけ。指す先は同じ |
| .dram0.data | 126524 | 126828 | +304 | owner object末尾に `PidDiagnostics`（sizeof 304、全0初期値）を追加。先頭1632 byteは同一（移動した文字列pointer 1個は同じ文字列）。後続の.data/.bss番地は+304移動 |
| .flash.text | 549635 | 550399 | +764 | 下記 |
| .flash.appdesc | 256 | 256 | 0 | 一致 |
| .flash.rodata | 176508 | 176508 | 0 | build SHA 40 byte、同じ30文字列のmerge順変更（strcmpで使う "LOAD_BUDGET_ESSENTIAL"/"QUERY_ADMISSION_FAILED" がstop()側の文字列sectionへ移動）、移動したcode番地（EH pc_begin、CIE personality、jump table）、S1関数とrelaxのみの関数の unwind/LSDA byte |

.flash.text の関数差分:

- 計数コード（許可）: `CanOwner::tick`（1456→1946）、`CanOwner::stop`（102→244、strcmp 2回とdeny/cancel）、`CanOwner::result`（999→1043、finish）、新規 `PidDiagnostics::add`（52 byte）・`PidDiagnostics::cancel()`（55 byte）。observe/admit/shedAux/deny/finishはインライン化。`pidDiagnostics()` read-outは製品コードから呼ばれないため機械語なし。
- 同じ翻訳単位 `signal_v2_gatt.cpp` の8関数はGCC出力が同一で、assembler/linkerのalignment・narrow/wide選択だけが異なる: `validProbe`・`probeResult`・`pin`・`reportS1`（GCC `-S` 命令列がbase/headで完全一致）、`grantCanArm`・`manage`・`poll`・`begin`（alignment fillとdensity形を同一視すると命令・分岐先・literal値が一致）。
- 他の全関数は再配置maskとsymbolic比較で一致（161 segmentは移動番地だけ）。literal pool 9領域はpool配置/共有の差で、使用側 `l32r` の値は関数ごとに一致を確認済み。
- owner_live の discovery symbol は base/head とも 0。

再現: base/headそれぞれの clone で `pio run -e m5dial_signal_v2_can_29bit_5hz_owner_live` と、clone外の `PLATFORMIO_BUILD_DIR` に `PLATFORMIO_BUILD_FLAGS=-Wl,--emit-relocs` で同envをbuildする。`pio run -v` の `src/signal_v2_gatt.cpp.o` compile行の `-c`/`-o` を `-S`/`-o *.s` に替えて GCC 出力を得る。その後 `python3 tools/test_pid_s1_load.py --base … --head … --base-relocs … --head-relocs … --asm-base … --asm-head … --tool-prefix …/xtensa-esp32s3-elf- --allow 'signal_v2::CanOwner::tick(…)' …`（許可は上記5関数だけ）。docs/tools だけを変更した最終headは、上記S1 code commitとbuild SHA文字列だけが異なることを別に確認する。

## 範囲外・未実施

exact request追加、予約再設計、owner_live 10Hz、実機serial HIL、bench CAN、車両、RF/画面、人間受入、flash/serial portアクセスは未実施。10Hz full予約が両上限に収まらない監査は先行文書に残す。分割要求/phase調整/確定分精算は別reviewとSTAGE_GATED labの検討対象で、今回は実装しない。実車GOは保留。
