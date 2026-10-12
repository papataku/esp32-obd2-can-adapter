#!/usr/bin/env python3
"""Download retained M5CAN trip records without clearing them (requires pyserial)."""
import argparse
from datetime import datetime
from pathlib import Path
import time
import serial


def collect(port, output, timeout=30):
    # USB opening may restart ESP32; prior NVS trips remain in the journal.
    link=serial.Serial()
    link.port=port
    link.baudrate=115200
    link.timeout=0.2
    link.dtr=False
    link.rts=False
    link.open()
    try:
        # Wait for startup, keeping the raw startup and dump evidence together.
        deadline=time.monotonic()+4
        with Path(output).open('wb') as stream:
            while time.monotonic()<deadline:
                stream.write(link.read(4096))
            link.write(b'DRIVELOG\n')
            deadline=time.monotonic()+timeout
            tail=b''
            while time.monotonic()<deadline:
                data=link.read(4096)
                stream.write(data)
                tail=(tail+data)[-4096:]
                if b'#DRIVELOG,END' in tail:
                    return
        raise RuntimeError('DRIVELOG response incomplete; partial raw log preserved')
    finally:
        link.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',required=True)
    parser.add_argument('--output',default='m5can-drive-'+datetime.now().strftime('%Y%m%d-%H%M%S')+'.log')
    args=parser.parse_args()
    collect(args.port,args.output)
    print(args.output)


if __name__=='__main__':
    main()
