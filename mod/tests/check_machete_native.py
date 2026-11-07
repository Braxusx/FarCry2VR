"""Compare actual left machete placement against the native right-hand reference."""
import argparse
from collections import defaultdict
from pathlib import Path
import math
import re

def mirror(matrix,normal,point):
    length=math.sqrt(sum(v*v for v in normal));assert length>.9, 'invalid authored blade plane'
    normal=[v/length for v in normal];distance=sum(a*b for a,b in zip(normal,point))
    reflection=[0.0]*16;reflection[15]=1
    for i in range(3):
        for j in range(3):reflection[i*4+j]=(1 if i==j else 0)-2*normal[i]*normal[j]
        reflection[12+i]=2*distance*normal[i]
    return [sum(reflection[i*4+k]*matrix[k*4+j] for k in range(4))*(-1 if j==0 else 1) for i in range(4) for j in range(4)]
def check_text(text):
    samples=defaultdict(list)
    planes=[];bones={}
    for line in text.splitlines():
        if 'melee-plane normal=' in line:
            r=dict(re.findall(r'(\w+)=([^\s]+)',line));planes.append(([float(v) for v in r['normal'].split(',')],[float(v) for v in r['point'].split(',')]))
        if 'melee-bone layout=' in line:
            r=dict(re.findall(r'(\w+)=([^\s]+)',line));bones[int(r['bone'])]=[float(v) for v in r['origin'].split(',')]
        if 'melee-render sample=' not in line:continue
        r=dict(re.findall(r'(\w+)=([^\s]+)',line));phase=int(r['sample'])//100000
        assert r['key']=='570988e9', 'not the observed Steam machete asset'
        matrix=list(map(float,r['relative'].split(',')))
        assert len(matrix)==16 and all(map(math.isfinite,matrix)), 'invalid blade transform'
        samples[phase,int(r['eye'])].append((r,matrix))
    assert planes and all(i in bones for i in [0,1,2]), 'missing authored blade-plane evidence'
    normal,point=planes[-1]
    long_axis=[b-a for a,b in zip(bones[0],bones[2])]
    denominator=math.sqrt(sum(v*v for v in long_axis)*sum(v*v for v in normal))
    assert abs(sum(a*b for a,b in zip(normal,long_axis)))/denominator<.25, 'reflection normal would reverse the long blade axis'
    maximum=0
    for eye in [0,1]:
        for phase in [22,23,24]:assert len(samples[phase,eye])>=3, f'phase {phase} eye {eye}: no native blade comparison'
        r,right=samples[23,eye][-1]
        assert r['left']=='0' and r['attachment']=='81', 'right reference not on the native right hand'
        expected=mirror(right,normal,point)
        assert max(abs(a-b) for a,b in zip(expected,right))>.2, 'reference cannot distinguish the old unmirrored fallback'
        for phase in [22,24]:
            for r,left in samples[phase,eye][-2:]:
                assert r['left']=='1' and r['attachment']=='56', 'left blade not on the anatomical left hand'
                error=max(abs(a-b) for a,b in zip(expected,left));maximum=max(maximum,error)
                assert error<.015, f'phase {phase} eye {eye}: blade differs from mirrored right-hand reference ({error})'
    return maximum

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path)
    args=parser.parse_args();error=check_text((args.run/'native.log').read_text(errors='replace'))
    print(f'PASS: both-eye left/right/left native machete placement, max matrix error {error:.6f}')
