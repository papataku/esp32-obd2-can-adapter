"""Re-review R1-R3 controls: fault/disconnect STOP correlation and overlap Snapshots."""
import copy
import pathlib
import sys
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import signal_v2_hil_ecu as h
from test_hil_evidence import trace,renumber,fault_trace


def healthy_prefix(until=.241):
    events=[e for e in trace() if e.get('t',99)<=until]
    for e in list(events):
        if e['kind']=='rx':events.append(dict(kind='write',text=h.rx_line(e['id'],e['data'],True,1),write_t=e['t']-.0001))
    return events


def adder(events,gen):
    def add(t,kind,**kw):
        events.append(dict(kind=kind,t=t,t_us=str(round(t*1e6)),boot='12345678',gen=gen,lost=0,text='',**kw))
    return add


def final_stop(add,epoch):
    add(60.1,'stop_request',epoch=str(epoch));add(60.11,'stopped',epoch=str(epoch))
    add(61.2,'fence',nonce='freshend')


def disconnect_trace():
    events=healthy_prefix();add=adder(events,1)
    add(.3,'disconnect',handle='1');add(.31,'stopped',epoch='2',source_generation='3',mode_epoch='3')
    final_stop(add,3)
    return renumber(events)


def overlap_trace():
    events=healthy_prefix();add=adder(events,2)
    add(.3,'stop_request',epoch='2');add(.31,'stopped',epoch='2',source_generation='3',mode_epoch='3')
    add(.5,'packet',type='33',epoch='2',fresh='0',current='1',source_generation='3',mode_epoch='3')
    add=adder(events,3)
    add(1.7,'arm',epoch='3',ok='1')
    for g in ('owner','monitor'):
        add(1.705,'gate',gate=g,epoch='3',cost='800',used100='800',used1000='4000',discovery='1')
    add(1.71,'tx',id=h.ECU_TX,ext=1,dlc=8,data=h.PROBE.ljust(8,b'\0'),ok=1)
    events.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.GROUP_CF[0],True,3),write_t=1.7109))
    add(1.711,'rx',id=h.ECU_RX,ext=1,dlc=8,data=h.GROUP_CF[0],ok=1)
    add(1.712,'fault',epoch='3',reason='DISCOVERY_RESPONSE_CONTRACT')
    add(1.72,'stopped',epoch='4',source_generation='4',mode_epoch='4')
    add(1.9,'packet',type='33',epoch='4',fresh='0',current='1',source_generation='4',mode_epoch='4')
    final_stop(add,5)
    return renumber(events)


class R66c(unittest.TestCase):
    def test_r1_fault_needs_its_own_completed_stop(self):
        events=[e for e in fault_trace('noreply') if not (e['kind']=='stopped' and e['t']==.21)]
        r=h.evaluate('noreply',renumber(events))
        self.assertFalse(r['checks']['stop_as_expected'],r)
        late=fault_trace('noreply')
        stop=next(e for e in late if e['kind']=='stopped' and e['t']==.21)
        stop['t']=3.5;stop['t_us']='3500000'
        self.assertFalse(h.evaluate('noreply',renumber(late))['checks']['stop_as_expected'])
        stale=fault_trace('noreply')
        next(e for e in stale if e['kind']=='stopped' and e['t']==.21)['epoch']='1'
        self.assertFalse(h.evaluate('noreply',stale)['checks']['stop_as_expected'])

    def test_r3_disconnect_positive_and_negative_controls(self):
        r=h.evaluate('disconnect',disconnect_trace());self.assertTrue(r['pass'],r)
        events=[e for e in disconnect_trace() if not (e['kind']=='stopped' and e['t']==.31)]
        self.assertFalse(h.evaluate('disconnect',renumber(events))['checks']['stop_as_expected'])

    def test_r2_overlap_requires_correlated_snapshots(self):
        r=h.evaluate('overlap',overlap_trace());self.assertTrue(r['pass'],r)
        for t in (.5,1.9):
            events=[e for e in overlap_trace() if not (e['kind']=='packet' and e['t']==t)]
            self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['epoch_separation'],t)
        stale=overlap_trace();add=adder(stale,3)
        add(1.7105,'packet',type='33',epoch='1',fresh='1',current='1',source_generation='1',mode_epoch='1')
        self.assertFalse(h.evaluate('overlap',renumber(stale))['checks']['epoch_separation'])

    def test_failed_fc_and_wrong_generation_rejected(self):
        e=trace();next(x for x in e if x['kind']=='tx' and x['data'][:1]==b'\x30')['ok']=0
        self.assertFalse(h.evaluate('normal',e)['pass'])
        e=trace()
        for x in e:
            if x['kind']=='write':x['text']=x['text'].replace('HIL RX 1 ','HIL RX 999 ',1)
        self.assertFalse(h.evaluate('normal',e)['checks']['transport_complete'])


if __name__=='__main__':unittest.main()
