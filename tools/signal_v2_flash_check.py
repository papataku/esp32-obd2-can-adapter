#!/usr/bin/env python3
"""Read-only app/partition/OTA gate. Never writes or erases device flash."""
import argparse,hashlib,json,struct,zlib
from pathlib import Path

def app_valid(b,offset=0):
 if b[offset]!=0xe9 or not 1<=b[offset+1]<=16:raise ValueError('invalid ESP image header')
 end=offset+24
 for _ in range(b[offset+1]):
  _,n=struct.unpack_from('<II',b,end);end+=8+n
  if end>len(b):raise ValueError('invalid segment length')
 end=(end+16)//16*16
 if hashlib.sha256(b[offset:end]).digest()!=b[end:end+32]:raise ValueError('ESP image SHA mismatch')
 return end+32-offset

def inspect(backup,firmware,target):
 b=Path(backup).read_bytes();fw=Path(firmware).read_bytes()
 if len(b)!={'m5dial':8,'smartring':16}[target]*1024*1024:raise ValueError('flash size mismatch')
 rows=[]
 for off in range(0x8000,0x9000,32):
  v=struct.unpack_from('<HBBII16sI',b,off)
  if v[0]!=0x50aa:break
  rows.append(dict(type=v[1],subtype=v[2],offset=v[3],size=v[4],name=v[5].rstrip(b'\0').decode(),flags=v[6]))
 apps=[r for r in rows if r['type']==0]
 if target=='m5dial':
  app=next(r for r in apps if r['subtype']==0)
  if app['offset']!=0x10000 or app['size']!=0x200000:raise ValueError('unexpected factory layout')
 else:
  valid=[]
  for off in (0xe000,0xf000):
   seq=struct.unpack_from('<I',b,off)[0];state,crc=struct.unpack_from('<II',b,off+24)
   if seq not in (0,0xffffffff) and state not in (3,4) and zlib.crc32(b[off:off+4],0xffffffff)==crc:valid.append(seq)
  if not valid:raise ValueError('OTA selection is unknown; refuse to choose an app')
  slot=(max(valid)-1)%2;app=next(r for r in apps if r['subtype']==0x10+slot)
  if app['size']!=0x640000:raise ValueError('unexpected SmartRing layout')
 app_valid(b,app['offset']);app_valid(fw)
 if len(fw)>app['size']:raise ValueError('firmware exceeds installed app partition')
 return dict(target=target,backup_sha256=hashlib.sha256(b).hexdigest(),firmware_sha256=hashlib.sha256(fw).hexdigest(),app=app,partitions=rows,partition_sector_sha256=hashlib.sha256(b[0x8000:0x9000]).hexdigest(),write='APP_ONLY',preserve=['bootloader','partition table','otadata','NVS','SPIFFS','CANLOG','other OTA app','coredump'])
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--backup',required=True);p.add_argument('--firmware',required=True);p.add_argument('--target',choices=['m5dial','smartring'],required=True);a=p.parse_args();print(json.dumps(inspect(a.backup,a.firmware,a.target),indent=2))
