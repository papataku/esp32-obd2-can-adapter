"""Board-trace oracle controls and adversarial evidence mutations (PR66 M2-4)."""
import copy
import pathlib
import sys
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import signal_v2_hil_ecu as h


def trace(scenario='normal'):
    events=[]
    def add(t,kind,**kw):
        events.append(dict(t=t,t_us=str(round(t*1e6)),kind=kind,gen=1,lost=0,boot="12345678",text='',**kw))
    def frame(t,kind,data,ok=1,cid=h.ECU_RX):
        if kind=='rx':
            events.append(dict(kind='write',text=h.rx_line(cid,data,generation=1)))
        add(t,kind,id=cid,ext=1,dlc=8 if kind=='tx' else len(data),data=data,ok=ok)
    def gate(t,discovery=False):
        for g in ('owner','monitor'):
            add(t,'gate',gate=g,epoch='1',cost='800' if discovery else '2400',
                used100='800' if discovery else '3200',used1000='800' if discovery else '14400',discovery='1' if discovery else '0')
    add(0,'fence',nonce='fresh');add(.01,'arm',epoch='1',ok='1')
    gate(.015,True);frame(.02,'tx',h.PROBE.ljust(8,b'\0'),cid=h.ECU_TX)
    frame(.025,'rx',h.PAIR_SF)
    for i in range(299):
        t=.22+i*.2
        gate(t)
        frame(t+.001,'tx',h.GROUP.ljust(8,b'\0'),cid=h.ECU_TX)
        frame(t+.006,'rx',h.GROUP_FF)
        frame(t+.007,'tx',b'\x30'+b'\0'*7,cid=h.ECU_TX)
        frame(t+.009,'rx',h.GROUP_CF[0]);frame(t+.011,'rx',h.GROUP_CF[1])
        add(t+.02,'sample',epoch='1',quality='0',speed='0',rpm='1000',source='18DAF101',
            observed_us=str(round((t+.011)*1e6)))
    # The runner records its final operator STOP write before the request.
    events.append(dict(kind='write',text='SIGV2 CAN STOP',write_t=60.099))
    add(60.1,'stop_request',epoch='2');add(60.11,'stopped',epoch='2');add(61.2,'fence',nonce='freshend')
    n=0
    for e in events:
        if 't_us' in e:
            n+=1;e['seq']=n
    return events


def renumber(events):
    # Host writes have no board clock; retain their relative insertion order.
    events.sort(key=lambda e:e.get('t',e.get('write_t',0)))
    seq=0
    for e in events:
        if 't_us' in e:
            seq+=1;e['seq']=seq
    return events


def fault_trace(scenario):
    template=trace()
    events=[e for e in template if e.get('t',99)<=.02]
    def add(t,kind,**kw):
        events.append(dict(kind=kind,t=t,t_us=str(round(t*1e6)),boot='12345678',gen=1,lost=0,text='',**kw))
    ecu=h.VirtualEcu(scenario)
    if scenario=='txfail':
        add(.005,'command',command='TXFAIL 1')
        next(e for e in events if e['kind']=='tx')['ok']=0
    for delay,line in ecu.on_tx(h.ECU_TX,h.PROBE):
        if not line.startswith('HIL RX '):continue
        _,_,_,cid,ext,data=line.split();data=bytes.fromhex(data)
        events.append(dict(kind='write',text=h.rx_line(int(cid,16),data,bool(int(ext)),1),write_t=.02+delay-.0001))
        add(.02+delay,'rx',id=int(cid,16),ext=int(ext),dlc=len(data),data=data,ok=1)
    add(.2,'fault',epoch='1',reason=sorted(h.FAULT_REASONS[scenario])[0])
    add(.21,'stopped',epoch='2')
    events.append(dict(kind='write',text='SIGV2 CAN STOP',write_t=60.099))
    add(60.1,'stop_request',epoch='3');add(60.11,'stopped',epoch='3')
    add(61.2,'fence',nonce='freshend')
    return renumber(events)


def rearm_trace():
    base=trace()
    events=[e for e in base if e.get('t',99)<=.241]
    for e in list(events):
        if e['kind']=='rx':events.append(dict(kind='write',text=h.rx_line(e['id'],e['data'],True,1),write_t=e['t']-.0001))
    def add(t,kind,**kw):
        events.append(dict(kind=kind,t=t,t_us=str(round(t*1e6)),boot='12345678',gen=2,lost=0,text='',**kw))
    add(.3,'stop_request',epoch='2');add(.31,'stopped',epoch='2',source_generation='3',mode_epoch='3')
    events.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.GROUP_CF[0],True,1),write_t=.399))
    add(.4,'rx',id=h.ECU_RX,ext=1,dlc=8,data=h.GROUP_CF[0],ok=0)
    add(.5,'packet',type='33',epoch='2',fresh='0',current='1',source_generation='3',mode_epoch='3')
    for e in trace():
        if e['kind']=='write':
            if e['text']=='SIGV2 CAN STOP':events.append(dict(e,write_t=e['write_t']+1.7))
            continue
        if e['kind']=='fence' and e['nonce']=='fresh':continue
        e=copy.deepcopy(e);e['t']+=1.7;e['t_us']=str(round(e['t']*1e6));e['gen']=3
        if 'observed_us' in e:e['observed_us']=str(int(e['observed_us'])+1700000)
        if 'epoch' in e:e['epoch']=str(int(e['epoch'])+2)
        events.append(e)
        if e['kind']=='rx':events.append(dict(kind='write',text=h.rx_line(e['id'],e['data'],True,3),write_t=e['t']-.0001))
    add(1.95,'packet',type='33',epoch='3',fresh='1',current='1',source_generation='4',mode_epoch='4',
        pdu_source_generation='4',pdu_mode_epoch='4',pdu_source='18DAF101',pdu_mixed='0')
    return renumber(events)


class Evidence(unittest.TestCase):
    def test_m2_discovery_fault_positive_controls(self):
        for scenario in ('noreply','extra','ff_probe','cf_probe','malformed','late','duplicate','wrong_src','eleven_bit','negative','txfail'):
            r=h.evaluate(scenario,fault_trace(scenario));self.assertTrue(r['pass'],r)
    def test_m2_missing_injection_and_late_stop_fail(self):
        for scenario in ('extra','ff_probe','cf_probe','malformed','late','duplicate','wrong_src','eleven_bit','negative','txfail'):
            events=fault_trace(scenario)
            without=[e for e in events if e['kind'] not in ('write','rx','command')]
            self.assertFalse(h.evaluate(scenario,renumber(without))['pass'],scenario)
            next(e for e in events if e['kind']=='fault')['t']=3.5
            self.assertFalse(h.evaluate(scenario,events)['checks']['stop_as_expected'],scenario)
    def test_m2_healthy_must_continue_until_stop(self):
        events=[e for e in trace() if e.get('t',0)<50 or e['kind'] in ('stop_request','stopped','fence')]
        self.assertFalse(h.evaluate('normal',renumber(events))['checks']['sustained_cadence'])
    def test_m2_exact_initial_rx_count(self):
        e=fault_trace('extra')
        rx=copy.deepcopy(next(x for x in e if x['kind']=='rx'));rx.update(t=.05,t_us='50000')
        e.append(rx);e.append(dict(kind='write',text=h.rx_line(rx['id'],rx['data'],True,1),write_t=.049))
        self.assertFalse(h.evaluate('extra',renumber(e))['checks']['scenario_exercised'])
    def test_healthy_complete_trace(self):
        r=h.evaluate('normal',trace());self.assertTrue(r['pass'],r)
    def test_m2_required_positive_evidence(self):
        for kind in ('arm','sample','gate','fence','stopped','write'):
            with self.subTest(kind=kind):
                self.assertFalse(h.evaluate('normal',[e for e in trace() if e['kind']!=kind])['pass'])
    def test_m2_tx_width_payload_and_functional_ids(self):
        for key,value in (('id',0x7DF),('id',0x18DB33F1),('ext',0),('data',b'\x02\x09\x02'+b'\0'*5),('dlc',7)):
            e=trace();next(x for x in e if x['kind']=='tx')[key]=value
            self.assertFalse(h.evaluate('normal',e)['pass'])
    def test_m2_wrong_fault_and_deadline(self):
        for scenario in h.SCENARIOS:
            if not h.SCENARIOS[scenario][0]: continue
            e=trace();e.insert(-2,dict(kind='fault',t=5,t_us='5000000',seq=999999,gen=1,lost=0,reason='RX_ACCOUNTING_OR_BUS_FAULT'))
            self.assertFalse(h.evaluate(scenario,e)['pass'])
    def test_m3_rearm_requires_old_cf_rejection_and_invalid_interval(self):
        evidence=rearm_trace();r=h.evaluate('rearm',evidence)
        self.assertTrue(r['pass'],r)
        accepted=copy.deepcopy(evidence)
        next(e for e in accepted if e['kind']=='rx' and e['ok']==0)['ok']=1
        self.assertFalse(h.evaluate('rearm',accepted)['checks']['epoch_separation'])
        stale=copy.deepcopy(evidence)
        next(e for e in stale if e['kind']=='packet' and e['fresh']=='1')['pdu_source_generation']='3'
        self.assertFalse(h.evaluate('rearm',stale)['checks']['epoch_separation'])
        late_invalid=copy.deepcopy(evidence)
        invalid=next(e for e in late_invalid if e['kind']=='packet' and e['fresh']=='0')
        invalid['t']=62.0;invalid['t_us']='62000000'
        self.assertFalse(h.evaluate('rearm',renumber(late_invalid))['checks']['epoch_separation'])
    def test_m3_stop_is_observed_until_quiet(self):
        e=trace();tx=copy.deepcopy(next(x for x in e if x['kind']=='tx'))
        tx.update(t=60.5,t_us='60500000',seq=e[-1]['seq']);e.insert(-1,tx);e[-1]['seq']+=1
        r=h.evaluate('normal',e);self.assertFalse(r['checks']['no_tx_after_stop'])
    def test_m3_fault_persists_on_second_probe(self):
        for s in ('extra','ff_probe','cf_probe','malformed','late','duplicate','wrong_src','eleven_bit','negative'):
            ecu=h.VirtualEcu(s);first=ecu.on_tx(h.ECU_TX,h.PROBE)
            self.assertEqual(first,ecu.on_tx(h.ECU_TX,h.PROBE),s)
    def test_m3_both_first_reservations_and_envelope(self):
        for gate in ('owner','monitor'):
            e=trace();next(x for x in e if x['kind']=='gate' and x['gate']==gate)['cost']='600'
            self.assertFalse(h.evaluate('normal',e)['checks']['both_admission_gates'])
        self.assertFalse(h.evaluate('admission',trace())['checks']['both_admission_gates'])
    def test_m4_loss_sequence_and_rx_ack_required(self):
        for mutation in ('loss','gap','ack','clock','error'):
            e=trace()
            if mutation=='loss': next(x for x in e if x['kind']=='rx')['lost']=1
            elif mutation=='gap': next(x for x in e if x['kind']=='rx')['seq']+=1
            elif mutation=='ack': next(x for x in e if x['kind']=='rx')['ok']=0
            elif mutation=='clock': next(x for x in e if x['kind']=='rx')['t']=-1
            else: e.append(dict(kind='line',text='#HIL_ERR,BAD_RX'))
            self.assertFalse(h.evaluate('normal',e)['checks']['transport_complete'],mutation)
    def test_m4_usb_batch_times_do_not_set_load(self):
        e=trace()
        for x in e:x['host_t']=0
        self.assertTrue(h.evaluate('normal',e)['pass'])
        # 26 board frames in 100ms cannot be hidden by spread USB batches.
        frames=[x for x in e if x['kind'] in ('tx','rx')][:26]
        for i,x in enumerate(frames): x['t']=i*.001;x['host_t']=i*.101
        self.assertFalse(h.evaluate('normal',e)['checks']['within_100ms_cap'])
    def test_m4_parser_rejects_bad_dlc(self):
        with self.assertRaises(ValueError):
            h.parse_event('#HIL_EVENT,seq=1,t_us=1,gen=1,lost=0,kind=tx,id=18DA01F1,ext=1,dlc=8,data=00,ok=1')

if __name__=='__main__':unittest.main()
