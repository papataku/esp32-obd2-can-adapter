# P5 検証記録（ホストのみ）

固定ベースは P3 merge `0424017`、設計は iOS `24fbd7d8308729ecfa69cae3cdc3fd402af1cff0`。
実機・シリアルポート・flash・物理 CAN TX は一切使用していない。

- `PATH=/opt/homebrew/opt/python@3.11/libexec/bin:$PATH tests/run_all.sh`: PASS。
- `docker run --rm -v "$PWD":/work -w /work -e ASAN_OPTIONS=detect_leaks=0 python:3.12 bash -c 'git config --global --add safe.directory /work; tests/run_all.sh'`: PASS。Python 3.12.16、GCC 14.2.0、警告0件。ASan leak 検出のみ、既存 host stub のプロセス終了時 allocation に対して無効化。境界/UB sanitizer は有効。
- `python3.11 tests/run_p5_discovery.py`: 制限/27シナリオ＋実 USB parser host HIL。TX は同じ CanOwner/CanMonitor から serial TWAI shim へだけ到達する。
- `python3.11 tools/test_p5_mutations.py`: ninth / FC / one-query / owner_live 隔離の4 mutation が assertion で失敗。一時コピーは自動削除。
- Docker `python tools/test_p5_mutations.py`: GCC14 でも4件検出、警告0件。
- `python3.11 tools/test_p5_scope.py`: 全27 env のプリプロセス検査。既存26 env の discovery=0、固定ベースとの token 同値。owner_live に discovery macro を加えると compile guard が拒否。
- `python3.11 tools/test_p3_scope.py`: P3 の wire/Session/GATT 契約を維持し、P5 guarded additions 除去後の固定ベース同値。
- P2 TX 同値性、P3 wire 同値性は両方の `run_all.sh` に含む。base commit 不在時には fetch する。
- PlatformIO 全27 env: PASS。secure lab は既存 CI と同じ合成 identity/fresh-flash acknowledgement、vehicle-trial は専用合成 build inputs を使用。実車の承認を示す値ではない。初回 matrix で trial guard が拒否した記録と、正しい build-only inputs による成功を両方保持。
- PlatformIO 既存 GCC8 は従来の `DiscoveryProfile::wellFormed` の `-Wtype-limits` を出す。既存画像を維持するため変更せず。GCC14 の警告0件とは分けて記録。

正確な最終 head、owner_live firmware.bin SHA256、最終 ELF 検査、固定 P3 ベースとの LOAD セクション比較は PR 本文と作業ディレクトリの `report.md` に記録する。build stamp が head SHA に依存するため、これらは clean commit 後に取得する。

NOT RUN: 物理 serial HIL、bench CAN、車両、flash、実車応答台数の保証。
実車TXは未承認。車両と条件を指定した別途書面の owner 承認なしに lab env を車載デバイスへ flash してはならない。
