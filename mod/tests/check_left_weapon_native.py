"""Check readback of anatomical hand bones in a visual-only native fixture run."""
import argparse
import collections
import math
from pathlib import Path
import re

def check(path):
    return check_text(path.read_text(errors='replace'))

def check_text(text):
    rows=[]
    for line in text.splitlines():
        if 'left-weapon-rig sample=' not in line:
            continue
        row=dict(re.findall(r'(\w+)=([^\s]+)',line))
        phase=int(row['sample'])//100000
        if phase<2:
            continue
        assert row['leftBone']=='56' and row['rightBone']=='81', 'unexpected native Steam hand bones'
        assert row['authority']=='0,0', 'visual fixture acquired controller action authority'
        assert float(row.get('palmError','inf'))<0.001, 'native left wrist retained a support-hand pose instead of mirrored dominant grip'
        vector=lambda key:tuple(map(float,row[key].split(',')))
        left=math.dist(vector('left'),vector('weapon'))
        right=math.dist(vector('right'),vector('support'))
        assert math.isfinite(left) and left<0.01, f'phase {phase}: native left wrist misses weapon target ({left} m)'
        assert math.isfinite(right) and right<0.01, f'phase {phase}: native right wrist misses reach-limited support target ({right} m)'
        rows.append((phase,left,right))
    phases=collections.Counter(r[0] for r in rows)
    for phase in [2,3,4,5,6,7,12,13,14,15]:
        assert phases[phase]>=3, f'phase {phase}: missing anatomical hand readback'
    assert 'left-weapon: attached to anatomical left bone 56 (native right 81)' in text, 'native weapon attachment not observed'
    rendered=collections.defaultdict(list)
    for line in text.splitlines():
        if 'left-weapon-render sample=' not in line:
            continue
        row=dict(re.findall(r'(\w+)=([^\s]+)',line))
        rendered[int(row['sample'])//100000,int(row['eye'])].append(row)
    for phase in [2,3,4,5,6,7,12,13,14,15]:
        for eye in [0,1]:
            observations=rendered[phase,eye]
            assert len(observations)>=3, f'phase {phase} eye {eye}: missing post-render weapon readback'
            for row in observations[-2:]:
                assert row['attachment']=='56', 'engine rendered weapon on the wrong hand bone'
                position=float(row['positionError']);rotation=float(row['rotationError'])
                assert math.isfinite(position) and position<0.03, f'phase {phase} eye {eye}: rendered gun position error {position} m'
                assert math.isfinite(rotation) and rotation<0.03, f'phase {phase} eye {eye}: rendered gun orientation error {rotation}'
                assert row.get('muzzleReady')=='1', 'no owned native muzzle readback'
                assert float(row.get('muzzleError','inf'))<0.03, 'shot origin misses rendered muzzle'
                assert float(row.get('barrelError','inf'))<0.01, 'shot direction misses rendered barrel'
                assert float(row.get('referenceError','inf'))<0.03, 'reticle/scope ray misses world muzzle after reference conversion'
                assert float(row.get('referenceForwardError','inf'))<0.01, 'reticle/scope direction misses world barrel'
    return len(rows),max(r[1] for r in rows),max(r[2] for r in rows)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run',type=Path)
    args=parser.parse_args()
    count,left,right=check(args.run/'native.log')
    print(f'PASS: {count} native hand samples; left weapon error {left:.6f} m, right support error {right:.6f} m; visual-only authority')
