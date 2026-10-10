# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate narrowly disabled oscillator controls in build scratch."""
import argparse
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--mode',choices=('no-wall-floor','bulk-credit'),required=True)
a=p.parse_args()
s=a.source.read_text(encoding='utf-8')

def replace_function(marker,body):
    global s
    if s.count(marker)!=1:raise SystemExit('Review control function marker: '+marker)
    begin=s.index('{',s.index(marker));end=begin+1;depth=1
    while depth and end<len(s):
        if s[end]=='{':depth+=1
        elif s[end]=='}':depth-=1
        end+=1
    if depth:raise SystemExit('Unbalanced control function')
    s=s[:begin]+body+s[end:]

if a.mode=='no-wall-floor':
    replace_function('uint64_t EeScheduler::wallClockTarget(', '{\n    (void)now;\n    return 0u;\n}')
else:
    replace_function('bool wallClockCreditBlocked(','''{
        (void)pendingTimers;
        (void)pendingInvocations;
        (void)threads;
        return false;
    }''')
    old='const uint64_t timerBoundary = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();'
    if s.count(old) not in (1,2):raise SystemExit('Review chronological wall credit boundaries')
    s=s.replace(old,'const uint64_t timerBoundary = std::numeric_limits<uint64_t>::max();')
    old='                if (m_pendingEeTimerInterrupts != 0u)\n                    return;'
    if s.count(old)!=1:raise SystemExit('Review chronological timer service return')
    s=s.replace(old,'')
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(s,encoding='utf-8')
