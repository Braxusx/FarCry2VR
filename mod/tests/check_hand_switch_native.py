"""Verify real F1 transitions, physical wrists and both native weapon attachments."""
import argparse
from collections import defaultdict
import math
from pathlib import Path
import re

def rows(text, marker):
    for line in text.splitlines():
        if marker in line:
            yield dict(re.findall(r'(\w+)=([^\s;]+)',line))

def check_text(text):
    events=list(rows(text,'weapon-hand changed left='))
    assert [int(r['left']) for r in events]==[0,1,0,1], 'missing, duplicate or incorrect F1 transitions'
    layouts=[int(r['layout']) for r in events]
    assert all(b==a+1 for a,b in zip(layouts,layouts[1:])), 'layout generations must advance once per transition'
    assert all(r.get('inputSynced')=='0' for r in events), 'old input lease remained current after a hand switch'
    generations=[int(r['inputGeneration']) for r in events]
    assert all(b>a for a,b in zip(generations,generations[1:])), 'input generations did not advance on hand changes'
    rig=defaultdict(list)
    right=defaultdict(list)
    left=defaultdict(list)
    cameras=defaultdict(list)
    for r in rows(text,'hand-switch-rig sample='):rig[int(r['sample'])//100000].append(r)
    for r in rows(text,'hand-switch-render sample='):right[int(r['sample'])//100000,int(r['eye'])].append(r)
    for r in rows(text,'left-weapon-render sample='):left[int(r['sample'])//100000,int(r['eye'])].append(r)
    for r in rows(text,'body-yaw-eye frame='):cameras[int(r['sample'])//100000].append(r)
    assert cameras[17], 'missing pre-switch camera baseline'
    baseline=cameras[17][-1]
    for phase,expected,layout in zip(range(18,22),[0,1,0,1],layouts):
        assert len(rig[phase])>=3, f'phase {phase}: missing wrist readback'
        for r in rig[phase][-2:]:
            assert int(r['left'])==expected and int(r['layout'])==layout, 'old arm frame applied to a new layout'
            assert r['leftBone']=='56' and r['rightBone']=='81', 'anatomical hand identities changed'
            assert r['authority']=='0,0', 'visual fixture acquired controller authority'
            for field in ['leftError','rightError']:
                error=float(r[field]);assert math.isfinite(error) and error<.01, 'physical wrist target mismatch'
        for eye in [0,1]:
            rendered=(left if expected else right)[phase,eye]
            assert len(rendered)>=3, f'phase {phase} eye {eye}: missing native render evidence'
            for r in rendered[-2:]:
                assert r['attachment']==('56' if expected else '81'), 'weapon retained the previous hand attachment'
                for field in ['positionError','rotationError']:
                    error=float(r[field]);assert math.isfinite(error) and error<.03, 'rendered gun does not match attachment'
                if expected:
                    for field in ['muzzleError','barrelError','referenceError','referenceForwardError']:
                        assert float(r.get(field,'inf'))<.03, 'left muzzle/reference ray changed after toggling'
                else:
                    assert int(r['layout'])==layout, 'stale right attachment observation'
                    assert float(r.get('nativeOffsetError','inf'))<.001, 'right hand retained the mirrored left offset'
        assert len(cameras[phase])>=3, 'missing body-yaw continuity evidence'
        for r in cameras[phase][-2:]:
            assert r['reference']==baseline['reference'], 'F1 recentered the body-yaw reference'
            delta=float(r['native'])-float(baseline['native'])
            assert abs(math.atan2(math.sin(delta),math.cos(delta)))<.01, 'F1 rotated the body'
    return len(events),sum(len(v) for v in rig.values())

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path)
    args=parser.parse_args();events,samples=check_text((args.run/'native.log').read_text(errors='replace'))
    print(f'PASS: {events} real F1 transitions, {samples} wrist samples, both-eye attachments/native offset restore and body-yaw continuity')
