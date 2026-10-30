"""Re-review R1-R3 controls: fault/disconnect STOP correlation and overlap Snapshots."""
import copy
import pathlib
import sys
import unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import signal_v2_hil_ecu as h
from test_hil_evidence import trace,renumber,fault_trace,rearm_trace,annotate


def healthy_prefix(until=.241):
    events=[e for e in trace() if e.get('t',99)<=until]
    for e in list(events):
        if e['kind']=='rx':events.append(dict(kind='write',text=h.rx_line(e['id'],e['data'],True,1),write_t=e['t']-.0001))
    return events


def adder(events,gen):
    def add(t,kind,**kw):
        events.append(dict(kind=kind,t=t,t_us=str(round(t*1e6)),boot='12345678',gen=gen,lost=0,text='',**kw))
    return add


def final_stop(add,epoch,events=None):
    if events is not None:
        events.append(dict(kind='write',text='SIGV2 CAN STOP',write_t=60.099))
    add(60.1,'stop_request',epoch=str(epoch));add(60.11,'stopped',epoch=str(epoch))
    add(61.2,'fence',nonce='freshend')


def disconnect_trace():
    events=healthy_prefix();add=adder(events,1)
    add(.3,'disconnect',handle='1');add(.31,'stopped',epoch='2',source_generation='3',mode_epoch='3')
    final_stop(add,3,events)
    return annotate(renumber(events))


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
    final_stop(add,5,events)
    return annotate(renumber(events))


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


class R66d(unittest.TestCase):
    """Second independent review B-01..B-04 (ChatGPT, 2026-10-30)."""
    def test_b01_final_operator_stop_is_required(self):
        events=[e for e in fault_trace('noreply') if not (e['kind'] in ('stop_request','stopped') and e['t']>=60)]
        r=h.evaluate('noreply',renumber(events))
        self.assertFalse(r['checks']['stop_ack_and_quiet'],r)
        unfinished=[e for e in fault_trace('noreply') if not (e['kind']=='stopped' and e['t']>=60)]
        self.assertFalse(h.evaluate('noreply',renumber(unfinished))['checks']['stop_ack_and_quiet'])

    def test_b02_old_cf_must_overlap_new_probe(self):
        events=overlap_trace()
        cf=next(e for e in events if e['kind']=='rx' and e['t']==1.711)
        cf['t']=1.8;cf['t_us']='1800000'
        self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['epoch_separation'])

    def test_b03_fresh_pdu_generations_not_owner_counters(self):
        stale=overlap_trace();add=adder(stale,3)
        add(1.7105,'packet',type='33',epoch='3',fresh='1',current='1',source_generation='4',mode_epoch='4',
            pdu_source_generation='2',pdu_mode_epoch='2',pdu_source='18DAF101',pdu_mixed='0')
        self.assertFalse(h.evaluate('overlap',renumber(stale))['checks']['epoch_separation'])
        missing=overlap_trace();add=adder(missing,3)
        add(1.7105,'packet',type='33',epoch='3',fresh='1',current='1',source_generation='4',mode_epoch='4')
        self.assertFalse(h.evaluate('overlap',renumber(missing))['checks']['epoch_separation'])

    def test_b04_scheduled_rx_refused_after_fault_stop_is_fail_closed(self):
        events=overlap_trace();add=adder(events,4)
        events.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.PAIR_SF,True,3),write_t=1.7249,
                           from_t=1.71,from_gen=3))
        add(1.725,'rx',id=h.ECU_RX,ext=1,dlc=len(h.PAIR_SF),data=h.PAIR_SF,ok=0)
        r=h.evaluate('overlap',renumber(events));self.assertTrue(r['pass'],r)
        before=overlap_trace();add=adder(before,3)
        before.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.PAIR_SF,True,3),write_t=1.7105))
        add(1.7106,'rx',id=h.ECU_RX,ext=1,dlc=len(h.PAIR_SF),data=h.PAIR_SF,ok=0)
        self.assertFalse(h.evaluate('overlap',renumber(before))['checks']['transport_complete'])

class R66e(unittest.TestCase):
    """Third independent review B-05..B-07 (ChatGPT, 2026-10-30)."""
    def test_b05_final_ack_epoch_must_match_request(self):
        events=fault_trace('noreply')
        next(e for e in events if e['kind']=='stopped' and e['t']==60.11)['epoch']='2'
        self.assertFalse(h.evaluate('noreply',events)['checks']['stop_ack_and_quiet'])

    def test_b06_fresh_pdu_source_must_be_pinned_ecu(self):
        for mutation in ('wrong','missing','mixed'):
            events=rearm_trace()
            packet=next(e for e in events if e['kind']=='packet' and e['fresh']=='1')
            if mutation=='wrong':packet['pdu_source']='18DAF102'
            elif mutation=='missing':del packet['pdu_source']
            else:packet['pdu_mixed']='1'
            self.assertFalse(h.evaluate('rearm',events)['checks']['epoch_separation'],mutation)

    def test_b07_unscheduled_refusal_is_not_fail_closed_evidence(self):
        for write in (dict(),dict(from_t=1.9,from_gen=3),dict(from_t=1.71,from_gen=4)):
            events=overlap_trace();add=adder(events,4)
            events.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.PAIR_SF,True,3),write_t=1.7249,**write))
            add(1.725,'rx',id=h.ECU_RX,ext=1,dlc=len(h.PAIR_SF),data=h.PAIR_SF,ok=0)
            self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['transport_complete'],write)

class R66f(unittest.TestCase):
    """Fourth independent review B-08..B-10 (ChatGPT, 2026-10-30)."""
    def test_b08_final_stop_needs_host_write(self):
        events=[e for e in fault_trace('noreply') if not (e['kind']=='write' and e['text']=='SIGV2 CAN STOP')]
        self.assertFalse(h.evaluate('noreply',events)['checks']['stop_ack_and_quiet'])

    def test_b09_refused_rx_must_be_the_modelled_response(self):
        events=overlap_trace();add=adder(events,4)
        events.append(dict(kind='write',text=h.rx_line(0x18DAF102,h.PAIR_SF,True,3),write_t=1.7249,
                           from_t=1.71,from_gen=3))
        add(1.725,'rx',id=0x18DAF102,ext=1,dlc=len(h.PAIR_SF),data=h.PAIR_SF,ok=0)
        self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['transport_complete'])

    def test_b10_disconnect_scheduled_refusal_is_fail_closed(self):
        events=disconnect_trace();add=adder(events,1)
        for g in ('owner','monitor'):
            add(.289,'gate',gate=g,epoch='1',cost='2400',used100='2400',used1000='17600',discovery='0')
        add(.29,'tx',id=h.ECU_TX,ext=1,dlc=8,data=h.GROUP.ljust(8,b'\0'),ok=1)
        events.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.GROUP_FF,True,1),write_t=.3149,
                           from_t=.29,from_gen=1))
        add(.315,'rx',id=h.ECU_RX,ext=1,dlc=len(h.GROUP_FF),data=h.GROUP_FF,ok=0)
        r=h.evaluate('disconnect',renumber(events));self.assertTrue(r['pass'],r)

class R66g(unittest.TestCase):
    """Fifth independent review B-11..B-13 (ChatGPT, 2026-10-30)."""
    def refused(self,cid,data,write_t,rx_t,inject_gen=3):
        events=overlap_trace();add=adder(events,4)
        events.append(dict(kind='write',text=h.rx_line(cid,data,True,inject_gen),write_t=write_t,
                           from_t=1.71,from_gen=3))
        add(rx_t,'rx',id=cid,ext=1,dlc=len(data),data=data,ok=0)
        return h.evaluate('overlap',renumber(events))

    def test_b11_admitted_response_consumes_its_slot(self):
        # The second probe schedules exactly one old CF, already admitted at 1.711.
        self.assertFalse(self.refused(h.ECU_RX,h.GROUP_CF[0],1.7249,1.725)['checks']['transport_complete'])

    def test_b12_host_write_must_precede_board_rx(self):
        self.assertFalse(self.refused(h.ECU_RX,h.PAIR_SF,1.730,1.725)['checks']['transport_complete'])

    def test_b13_injection_generation_may_change_after_stop(self):
        r=self.refused(h.ECU_RX,h.PAIR_SF,1.7249,1.725,inject_gen=4)
        self.assertTrue(r['pass'],r)

class R66h(unittest.TestCase):
    """Sixth independent review M-01, M-02, m-01 (ChatGPT, 2026-10-30)."""
    def test_m01_admitted_rx_uses_its_own_write_generation(self):
        events=rearm_trace()
        cf=next(e for e in events if e['kind']=='rx' and e['ok']==1 and e['data']==h.GROUP_CF[0])
        write=next(w for w in events if w['kind']=='write' and w['text']==h.rx_line(h.ECU_RX,h.GROUP_CF[0],True,1)
                   and w.get('write_t',0)<cf['t'])
        write['text']=h.rx_line(h.ECU_RX,h.GROUP_CF[0],True,2)
        self.assertFalse(h.evaluate('rearm',events)['checks']['transport_complete'])

    def test_m02_m12_late_rx_after_stop_is_received_but_never_fresh(self):
        # M-12 supersedes M-02: the shim admits while its driver runs, even
        # after the logical STOP; that frame must never become Fresh data.
        events=overlap_trace();add=adder(events,4)
        events.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.PAIR_SF,True,4),write_t=1.7249,
                           from_t=1.71,from_gen=3))
        add(1.725,'rx',id=h.ECU_RX,ext=1,dlc=len(h.PAIR_SF),data=h.PAIR_SF,ok=1)
        r=h.evaluate('overlap',renumber(copy.deepcopy(events)));self.assertTrue(r['pass'],r)
        add(1.73,'packet',type='33',epoch='4',fresh='1',current='1',source_generation='5',mode_epoch='5',
            pdu_source_generation='5',pdu_mode_epoch='5',pdu_source='18DAF101',pdu_mixed='0')
        self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['epoch_separation'])

    def test_m01b_only_one_deliberate_rearm_injection(self):
        events=rearm_trace();add=adder(events,2)
        events.append(dict(kind='write',text=h.rx_line(0x18DAF102,h.PAIR_SF,True,1),write_t=.449,deliberate=True))
        add(.45,'rx',id=0x18DAF102,ext=1,dlc=len(h.PAIR_SF),data=h.PAIR_SF,ok=0)
        self.assertFalse(h.evaluate('rearm',renumber(events))['checks']['transport_complete'])

class R66i(unittest.TestCase):
    """Seventh independent review M-03, M-04 (ChatGPT, 2026-10-30)."""
    def deliberate(self,events):
        return next(w for w in events if w.get('deliberate'))

    def test_m03_deliberate_old_cf_has_race_generation(self):
        events=rearm_trace()
        w=self.deliberate(events);w['text']=w['text'].replace('HIL RX 1 ','HIL RX 999 ',1)
        self.assertFalse(h.evaluate('rearm',events)['checks']['transport_complete'])

    def test_m03_deliberate_write_follows_completed_stop(self):
        events=rearm_trace();w=self.deliberate(events)
        events.remove(w);w['write_t']=.305
        events.append(w)
        self.assertFalse(h.evaluate('rearm',renumber(events))['checks']['transport_complete'])

    def overlap_with_driver(self,*kinds):
        events=overlap_trace();add=adder(events,3)
        for i,k in enumerate(kinds):add(1.7101+i*.0001,k)
        return renumber(events)

    def test_m04_no_admission_while_driver_stopped(self):
        self.assertFalse(h.evaluate('overlap',self.overlap_with_driver('stop'))['checks']['transport_complete'])

    def test_m04_driver_restart_is_running_again(self):
        # A real reinstall before the re-ARM: stop -> uninstall (gen 2),
        # install -> start (gen 3); admissions then carry generation 3.
        events=overlap_trace()
        for t,k,g in ((1.69,'stop',2),(1.691,'uninstall',2),(1.692,'install',3),(1.693,'start',3)):
            adder(events,g)(t,k)
        r=h.evaluate('overlap',renumber(events));self.assertTrue(r['pass'],r)

class R66j(unittest.TestCase):
    """Eighth independent review M-05, M-06 (ChatGPT, 2026-10-30)."""
    def test_m05_race_group_must_be_pending_at_race_stop(self):
        events=rearm_trace()
        # The second group completes (final CF + sample) before the race STOP.
        add=adder(events,1)
        events.append(dict(kind='write',text=h.rx_line(h.ECU_RX,h.GROUP_CF[1],True,1),write_t=.4229,
                           from_t=.421,from_gen=1))
        add(.423,'rx',id=h.ECU_RX,ext=1,dlc=8,data=h.GROUP_CF[1],ok=1)
        add(.424,'sample',epoch='1',quality='0',speed='0',rpm='1000',source='18DAF101',observed_us='423000')
        self.assertFalse(h.evaluate('rearm',renumber(events))['checks']['epoch_separation'])

    def test_m06_admission_uses_the_installed_generation(self):
        events=overlap_trace()
        for t,k,g in ((1.7101,'stop',3),(1.7102,'uninstall',3),(1.7103,'install',4),(1.7104,'start',4)):
            adder(events,g)(t,k)
        self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['transport_complete'])

    def test_m06_impossible_driver_transition_cannot_admit(self):
        events=overlap_trace()
        for t,k in ((1.7101,'stop'),(1.7102,'install'),(1.7103,'start')):
            adder(events,3)(t,k)
        self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['transport_complete'])

class R66k(unittest.TestCase):
    """Ninth independent review M-07, M-08 (ChatGPT, 2026-10-30)."""
    def restart(self,start_gen):
        events=overlap_trace()
        for t,k,g in ((1.69,'stop',2),(1.691,'uninstall',2),(1.692,'install',3),(1.693,'start',start_gen)):
            adder(events,g)(t,k)
        return h.evaluate('overlap',renumber(events))

    def test_m07_lifecycle_event_generation_must_match(self):
        self.assertTrue(self.restart(3)['pass'])
        self.assertFalse(self.restart(2)['checks']['transport_complete'])

    def test_m07_install_must_advance_generation(self):
        events=overlap_trace()
        for t,k,g in ((1.69,'stop',3),(1.691,'uninstall',3),(1.692,'install',3),(1.693,'start',3)):
            adder(events,g)(t,k)
        self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['transport_complete'])

    def test_m08_successful_tx_requires_running_driver(self):
        events=overlap_trace()
        adder(events,3)(1.709,'stop');adder(events,3)(1.7105,'start')
        self.assertFalse(h.evaluate('overlap',renumber(events))['checks']['tx_allowlist'])
        ok=overlap_trace()
        adder(ok,3)(1.708,'stop');adder(ok,3)(1.709,'start')
        r=h.evaluate('overlap',renumber(ok));self.assertTrue(r['pass'],r)

class R66l(unittest.TestCase):
    """Tenth independent review M-09..M-11 (ChatGPT, 2026-10-30)."""
    def test_m09_fc_during_discovery_fails_in_every_scenario(self):
        events=trace();add=adder(events,1)
        add(.03,'tx',id=h.ECU_TX,ext=1,dlc=8,data=b'\x30'+b'\0'*7,ok=1)
        self.assertFalse(h.evaluate('normal',renumber(events))['checks']['no_fc_on_probe_ff'])

    def test_m10_group_reservation_must_be_2400_before_its_tx(self):
        events=trace()
        for g in [e for e in events if e['kind']=='gate' and e['cost']=='2400'][:2]:
            g['cost']='800'
        self.assertFalse(h.evaluate('normal',events)['checks']['both_admission_gates'])
        late=trace()
        g=next(e for e in late if e['kind']=='gate' and e['cost']=='2400' and e['gate']=='owner')
        tx=next(e for e in late if e['kind']=='tx' and e['seq']>g['seq'])
        g['t']=tx['t']+.0001;g['t_us']=str(round(g['t']*1e6))
        self.assertFalse(h.evaluate('normal',renumber(late))['checks']['both_admission_gates'])

    def test_m11_write_before_its_cause_tx_fails(self):
        events=trace()
        w=next(e for e in events if e['kind']=='write' and e.get('from_t')==.02)
        events.remove(w);events.insert(0,w)
        self.assertFalse(h.evaluate('normal',events)['checks']['transport_complete'])

if __name__=='__main__':unittest.main()
