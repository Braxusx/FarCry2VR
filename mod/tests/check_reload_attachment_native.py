"""Check the owned RPG attachment replay; this is not a real reload-animation test."""
import argparse
from collections import defaultdict
from pathlib import Path
import math
import re

def check_text(text):
    for phase in [26,27]:assert f'reload-attachment-replay phase={phase} key=19a67e02' in text, 'real held RPG replay not exercised'
    samples=defaultdict(list)
    for line in text.splitlines():
        if 'reload-render sample=' not in line:continue
        r=dict(re.findall(r'(\w+)=([^\s]+)',line));phase=int(r['sample'])//100000
        assert r['key']=='19a67e02' and r['left']=='1', 'replay selected the wrong asset/hand'
        assert r['authority']=='0,0', 'attachment fixture acquired real action authority'
        matrix=list(map(float,r['relative'].split(',')))
        assert len(matrix)==16 and all(map(math.isfinite,matrix)), 'invalid RPG transform'
        samples[phase,int(r['eye'])].append((r,matrix))
    maximum=0
    for eye in [0,1]:
        assert len(samples[25,eye])>=3, 'missing native RPG rest'
        baseline=samples[25,eye][-1][1]
        for phase in [26,27,28]:
            assert len(samples[phase,eye])>=3, f'phase {phase} eye {eye}: missing RPG replay evidence'
            for r,matrix in samples[phase,eye][-2:]:
                assert r['attachment']=='56', 'RPG escaped to a native right/camera parent'
                error=max(abs(a-b) for a,b in zip(matrix,baseline));maximum=max(maximum,error)
                assert error<.015, f'phase {phase} eye {eye}: reload offset poisoned the native rest ({error})'
    return maximum

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path)
    args=parser.parse_args();error=check_text((args.run/'native.log').read_text(errors='replace'))
    print(f'PASS: both-eye owned RPG attachment replay/rest recovery, max matrix error {error:.6f}; no actual reload or synthetic shots')
