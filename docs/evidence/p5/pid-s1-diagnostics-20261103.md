# PID expansion S1 — read-only diagnostics

[計画](https://github.com/papataku/tacotaco-meter-ios/blob/4be832d2d0aac9bc4b650a63145c3b53a7f4efe7/docs/protocol/pid-expansion-plan.md) §10 item 1。固定adapter baseは `3e43dfe440d800b1ec82c17b6749e6e1ca322314`。

PM Claude 2026-11-03の決定により、adapter内のP3/P5 scopeだけに、以下の完全一致allowlistを追加する。iOS Public/Personal、Golden、liveAllowed、既存legacy-equivalence試験には変更しない。

- 診断ファイル: `include/signal_v2_pid_diagnostics.h`。
- 製品側追加symbol: `signal_v2::PidDiagnostics`（Count、pid、active_ms、last、due、epoch、active、pending、essential、add、observe、admit、finish、cancel、shedAux、deny）、`CanOwner::pid_diagnostics_`、`CanOwner::pidDiagnostics`。
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

`python3 tools/test_pid_s1.py` は新カウンタの独立期待値と、completion欠落・essentialを全PID成功に誤計数・shed欠落・busyの需要過小計数・cancel欠落・admit後だけ需要計数・飽和wrapの7 mutationを実行する。compile失敗を検出成功に数えない。変更対象は一時コピーだけ。

固定baseはshallow checkoutでも必要時にfetchする。既存owner_live、5Hz cadence、10Hz lab cadence、owner異常のfixtureに、tick時刻/epoch/需要・要求byte/ID/deadline・完成PDU/RAW RXを記録する処理を一時的に加え、base/通常/極端counter版のtraceを比較する。既存production monitorのM1/M2/M3/M6/M7/m1/controls/R1/R2/R3は別プロセスで走らせ、実driver境界の全TX（FC含む）/RX byte・時刻をbaseと比較する。元のfixtureとlegacy-equivalence検査は編集しない。

新しい120ms delay/busy/同時需要/STOP/overrun/uncertain admission/invalid試験、rolling予約の219/220msと1119/1120ms境界に加え、既存suiteのFC/all-RX/uncertain retry/STOP実driver境界検査をすべて維持する。

全suite: Python 3.11 + clang と `docker run --rm -v "$PWD:/work" -w /work python:3.12 bash -c './tests/run_all.sh'`（GCC 14）。全envはplatformio.iniの27 `env:`を明示列挙してpio runする（defaultのm5dialだけでは全env合格としない）。

## LOAD差分を説明する方法

`tools/test_pid_s1_load.py --base BASE_ELF --head HEAD_ELF --erased CONTROL_ELF --tool-prefix PREFIX`。

CONTROLは診断追加を除去したhead（owner/run_allが固定baseと一致）を同じenv・toolchainでビルドしたもの。最初の監査commit `ec17bbaaa55d9dfd6cebba133ee8050605112798` は文書だけの変更で、この対照に使える。baseとCONTROLは `.flash.rodata` の40-byte build SHA文字列1個を正規化すると、全6 LOADのVMA/LMA/長さ/全byteが一致する。

通常headはcounter storage・診断更新code・const read-outを加える。section長、配置、hashとowner/診断symbolサイズを記録し、増分本体に伴うlinker relocation/alignment、build identityを区別する。診断を除いたときに全LOADが戻ること、scope完全復元と実行trace一致を組み合わせて差の由来を限定する。通常headのLOADがbaseとbyte一致するとは主張しない。owner_liveのdiscovery symbolは0を要求する。

## 範囲外・未実施

exact request追加、予約再設計、owner_live 10Hz、実機serial HIL、bench CAN、車両、RF/画面、人間受入、flash/serial portアクセスは未実施。10Hz full予約が両上限に収まらない監査は先行文書に残す。分割要求/phase調整/確定分精算は別reviewとSTAGE_GATED labの検討対象で、今回は実装しない。実車GOは保留。
