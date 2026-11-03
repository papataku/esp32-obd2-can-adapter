#!/usr/bin/env python3
"""S1 exact read-only diagnostic allowlist; PM Claude decision 2026-11-03.
No marker ranges, wildcards or alternate TX/control-flow bodies are allowed.
"""
import pathlib, subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
BASE='3e43dfe440d800b1ec82c17b6749e6e1ca322314'
HOOKS=[('#include "signal_v2_can_budget.h"\n', '#include "signal_v2_pid_diagnostics.h"\n'),
 ('    in_flight_=true;\n  }\n',
  'private:\n'
  '  PidDiagnostics pid_diagnostics_{}; // Append storage: preserve existing member offsets.\n'),
 ('  void stop(const char* reason,uint8_t quality=10){\n',
  '    if(!std::strcmp(reason,"LOAD_BUDGET_ESSENTIAL"))pid_diagnostics_.deny(true);\n'
  '    '
  'if(!std::strcmp(reason,"QUERY_ADMISSION_FAILED")&&ecu_.fixed())pid_diagnostics_.deny(essential_);\n'
  '    pid_diagnostics_.cancel();\n'
  '    pid_diagnostics_.active=false;\n'),
 ('    ecu_.observeSingle(valid,r.status);\n', '    pid_diagnostics_.finish(valid);\n'),
 ('  const CanSamples &samples()const{return samples_;}\n',
  '  const PidDiagnostics &pidDiagnostics()const{return pid_diagnostics_;}\n'),
 ('        can_->beginObservationAudit(s1_base_);\n      }\n    }\n',
  '    '
  'pid_diagnostics_.observe(now,demand,armed&&!stopped_&&!s1_,epoch_);\n'),
 ('      essential_=true;req.signal_v2_essential=true;req.length=3;\n',
  '      pid_diagnostics_.shedAux();\n'),
 ('    physical_=!discovery;\n', '    if(!discovery)pid_diagnostics_.admit(essential_);\n')]
NEW={'include/signal_v2_pid_diagnostics.h','tools/test_pid_s1_scope.py',
     'tools/test_pid_s1.py','tools/test_pid_s1_load.py','tests/pid_s1.cpp'}
HEADER_SHA256='8b81a64b57f574d69fddf7f81662e55f091cca9ba3d0519eb419f38e16aa70c0'
RUN='\n# PID S1 read-only diagnostics, PM Claude 2026-11-03.\npython3 tools/test_pid_s1_scope.py\npython3 tools/test_pid_s1.py\n'
def strip_s1(path,text):
    if path=='src/signal_v2_can_owner.h':
        for anchor,addition in HOOKS:
            assert text.count(anchor+addition)==1,('S1 exact hook',anchor)
            text=text.replace(anchor+addition,anchor,1)
    if path=='tests/run_all.sh':
        assert text.endswith(RUN),'S1 test registration changed'
        text=text[:-len(RUN)]
    return text

def check():
    if subprocess.run(['git','cat-file','-e',BASE+'^{commit}'],cwd=ROOT,capture_output=True).returncode:
        subprocess.run(['git','fetch','--no-tags','origin',BASE],cwd=ROOT,check=True)
    for path in ('src/signal_v2_can_owner.h','tests/run_all.sh'):
        old=subprocess.check_output(['git','show',BASE+':'+path],cwd=ROOT,text=True)
        assert strip_s1(path,(ROOT/path).read_text())==old,path
    # All counter writes are isolated from scheduling: owner additions are the
    # exact side-effect-only calls above plus a const diagnostic read-out.
    diagnostic=(ROOT/'include/signal_v2_pid_diagnostics.h').read_text()
    import re
    assert not re.search(r'CanMonitor|CanBudget|submitQuery|transmit|acquireLease|revokeLease|Serial|#include "',diagnostic)
    assert 'const PidDiagnostics &pidDiagnostics()const' in (ROOT/'src/signal_v2_can_owner.h').read_text()
    # The header defines exactly the named symbols and nothing else.
    assert re.findall(r'\bstruct (\w+)',diagnostic)==['PidDiagnostics','Count'],'S1 struct set'
    methods=re.findall(r'^\s*(?:template<[^>]*>\s*)?(?:static\s+)?void (\w+)\(',diagnostic,re.M)
    assert methods==['add','cancel','observe','admit','finish','shedAux','deny'],methods
    # Members/fields are pinned by content: any header change must update this
    # reviewed digest (PM Claude 2026-11-03 exact allowlist).
    import hashlib
    assert hashlib.sha256(diagnostic.encode()).hexdigest()==HEADER_SHA256,'S1 diagnostics header changed'
    # Only const/value parameters reach the diagnostics: no owner pointer/reference escapes.
    assert not re.search(r'\bthis\b|CanOwner\s*[&*]|\bfriend\b',diagnostic)
    # A TX/request mutation outside the exact hooks cannot be laundered.
    text=(ROOT/'src/signal_v2_can_owner.h').read_text().replace('req.length=6;', 'req.length=5;',1)
    assert strip_s1('src/signal_v2_can_owner.h',text)!=subprocess.check_output(['git','show',BASE+':src/signal_v2_can_owner.h'],cwd=ROOT,text=True)
    print('PASS S1 exact symbols PidDiagnostics, pid_diagnostics_, pidDiagnostics; stripped owner/run_all identical to fixed base')
if __name__=='__main__':check()
