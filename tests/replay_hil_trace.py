#!/usr/bin/env python3
"""Replay private JSONL locally without copying raw evidence into Git."""
import argparse
import json
import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import signal_v2_hil_ecu as h


def replay(path):
    events = [json.loads(line) for line in pathlib.Path(path).read_text().splitlines()]
    for event in events:
        if event['kind'] in ('tx', 'rx') and isinstance(event.get('data'), str):
            event['data'] = bytes.fromhex(event['data'])
    return h.evaluate('normal', events)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', nargs='+')
    parser.add_argument('--check-epochs', action='store_true',
                        help='fail if any epoch_separation check fails; overall verdict is retained')
    args = parser.parse_args()
    results = {pathlib.Path(p).name: replay(p) for p in args.traces}
    print(json.dumps(results, indent=2))
    if args.check_epochs and not all(r['checks']['epoch_separation'] for r in results.values()):
        sys.exit(1)
