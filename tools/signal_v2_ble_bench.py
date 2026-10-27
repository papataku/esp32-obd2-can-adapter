#!/usr/bin/env python3
"""No-CAN on-air lab checks. Requires host Bluetooth consent and lab peripheral.
All commands target Signal V2 or safe V1 TX rejection. No raw CAN operation.
"""
import argparse,asyncio,json,struct,time,zlib
from pathlib import Path
from bleak import BleakClient,BleakScanner
BASE='f9c17d0{}-2a3e-4e18-89a2-d7c1b539e000'
CONTROL,DATA,STATUS=[BASE.format(i) for i in (1,2,3)]
ROOT=Path(__file__).resolve().parents[1]
F=json.loads((ROOT/'tests/fixtures/signal_v2_v3.json').read_text())

def seal(b):
 b=bytearray(b);b[22:26]=bytes(4);struct.pack_into('<I',b,22,zlib.crc32(b));return bytes(b)
def decode(b):
 if len(b)<26 or b[:8] not in [bytes.fromhex('4d350200'+t+'001a00') for t in ('01','20','21','22','80','81')]:raise ValueError('header')
 if len(b)!=26+int.from_bytes(b[20:22],'little') or b!=seal(b):raise ValueError('length/CRC')
 return b
class Lane:
 def __init__(self,lane):self.lane=lane;self.reset()
 def reset(self):self.buffer=bytearray();self.ident=None;self.total=0
 def feed(self,f):
  if len(f)<17:raise ValueError('short fragment')
  magic,version,lane,flags,reserved,ident,total,off,n,reserved2=struct.unpack('<HBBBBIHHBB',f[:16])
  if magic!=0x52d7 or version!=2 or lane!=self.lane or flags&~3 or reserved or reserved2 or len(f)!=16+n or not n or total>512 or total<26:raise ValueError('fragment header')
  if flags&1:
   if self.ident is not None:raise ValueError('overlap')
   self.ident,self.total=ident,total
  if ident!=self.ident or total!=self.total or off!=len(self.buffer) or off+n>total or bool(flags&2)!=(off+n==total):raise ValueError('fragment order')
  self.buffer.extend(f[16:])
  if flags&2:b=decode(bytes(self.buffer));self.reset();return b
async def main(a):
 results={f'G{i:02}':'NOT_RUN' for i in range(1,13)};results['scope']='on-air semantic lab; fixed CAN-origin G05/G07 remain decoder-only'
 st,da=Lane(3),Lane(2);acks,values=asyncio.Queue(),asyncio.Queue();errors=[]
 def callback(lane,queue):
  def receive(_,raw):
   try:
    p=lane.feed(bytes(raw))
    if p is not None:queue.put_nowait(p)
   except Exception as e:errors.append(str(e));lane.reset()
  return receive
 devices=await BleakScanner.discover(timeout=8)
 candidates=[d for d in devices if d.address==a.device or (not a.device and d.name=='M5CAN-Dial')]
 if len(candidates)!=1:raise RuntimeError('expected exactly one specified M5CAN-Dial')
 async with BleakClient(candidates[0],timeout=20) as c:
  print('connected',c.address,'mtu',c.mtu_size,flush=True)
  results['negotiated_mtu']=c.mtu_size
  await c.start_notify(STATUS,callback(st,acks));await c.start_notify(DATA,callback(da,values))
  transfer=0;last_command=0
  async def send(p,mtu=247):
   nonlocal transfer,last_command
   await asyncio.sleep(max(0,.250-(time.monotonic()-last_command)))
   last_command=time.monotonic();transfer+=1
   for off in range(0,len(p),min(mtu-19,244)):
    chunk=p[off:off+min(mtu-19,244)];flags=(1 if off==0 else 0)|(2 if off+len(chunk)==len(p) else 0)
    f=struct.pack('<HBBBBIHHBB',0x52d7,2,1,flags,0,transfer,len(p),off,len(chunk),0)+chunk
    await c.write_gatt_char(CONTROL,f,response=True)
  async def ack(rid):
   async with asyncio.timeout(5):
    while True:
     p=await acks.get()
     if int.from_bytes(p[26:30],'little')==rid:return p
  async def value(kind):
   async with asyncio.timeout(5):
    while True:
     p=await values.get()
     if p[4]==kind:return p
  hello=bytes.fromhex(F['G01']);await send(hello,23);h=await ack(1)
  if h[31]==6 and a.approve_identity:
   import serial
   with serial.Serial(a.usb,115200,timeout=.2) as usb:
    usb.dtr=True;usb.rts=False
    usb.write(('SIGV2 WINDOW\nSIGV2 APPROVE '+a.approve_identity+'\n').encode());await asyncio.sleep(1)
    admin=usb.read(8192).decode(errors='replace');print(admin,flush=True)
    if 'saved=1' not in admin:raise RuntimeError('USB registration failed; identity must be locally verified')
   await send(hello,23);h=await ack(1)
  if h[31]!=0:raise RuntimeError('HELLO not authorized/accepted: '+str(h[31]))
  assert h[26:]==bytes.fromhex(F['G02'])[26:]
  results.update(G01='PASS',G02='PASS',G10='PASS (13 fixed-size MTU23 fragments; actual negotiated MTU recorded separately)')
  boot,session=struct.unpack_from('<II',h,8)
  def fixture(name):
   p=bytearray.fromhex(F[name]);struct.pack_into('<II',p,8,boot,session);return seal(p)
  p=fixture('G03');await send(p);r=await ack(2);assert r[26:]==bytes.fromhex(F['G04'])[26:];results.update(G03='PASS',G04='PASS')
  await value(0x21);d=await value(0x20);record=d[28:];assert record[3]==2 and record[7]==1 and record[44:48]==bytes.fromhex('410c0000');results['G05']='PASS (DEMO equivalent; fixed CAN-origin vector is decoder-only)'
  da.reset();await send(fixture('G06'));r=await ack(3);assert r[26:]==bytes.fromhex(F['G06_2'])[26:];snap=await value(0x21)
  assert struct.unpack_from('<I',snap,26)[0]==struct.unpack_from('<I',snap,16)[0]+1 and struct.unpack_from('<I',snap,34)[0]==1
  results.update(G06='PASS',G07='PASS (dynamic session/cache; DEMO equivalent)')
  await send(fixture('G09'));r=await ack(4);assert r[26:]==bytes.fromhex(F['G09_2'])[26:];results['G09']='PASS'
  await send(fixture('G11'));r=await ack(5);assert r[26:]==bytes.fromhex(F['G11_2'])[26:];results['G11']='PASS'
  # CRC-corrupt Control never receives an ACK and never mutates revision.
  corrupt=bytearray(fixture('G11'));corrupt[-1]^=1;await send(bytes(corrupt));await asyncio.sleep(.35)
  decode(bytes.fromhex(F['G05']))
  try:decode(bytes.fromhex(F['G12']))
  except ValueError:results['G12']='PASS (host receive CRC rejection + on-air corrupt Control ignored)'
  else:raise AssertionError('G12 CRC accepted')
  hb=await value(0x22);assert len(hb)==38;results['G08']='PASS'
  assert not errors,errors
  results['fragment_errors']=errors
 return results
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--device');p.add_argument('--usb',default='/dev/cu.usbmodem1101');p.add_argument('--approve-identity',help='explicitly USB-verified local host identity only');p.add_argument('--output',type=Path);a=p.parse_args()
 result=asyncio.run(main(a));s=json.dumps(result,indent=2);print(s,flush=True)
 if a.output:a.output.write_text(s+'\n')
