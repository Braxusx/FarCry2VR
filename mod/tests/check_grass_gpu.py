"""Check bound DX10 grass upload bytes in the native diagnostic capture.

Steam Dunia 6353936a...862765: these captured VS families use cb1's reflected
_GrassCylindricalBillboardMatrix (512:576, provider size 1008). N records are
native uploads; B records are the actual bytes passed to Unmap/UpdateSubresource.
"""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import struct

GRASS = {'ffc5e0595cbbd46d', '08a95cac2f0c797e'}
YAW = {2:0, 3:90, 4:-90, 5:179, 6:-179, 7:-179, 14:45, 15:90}

def records(path):
    data=path.read_bytes();p=0
    while p<len(data):
        if p+5>len(data):raise ValueError('truncated record header')
        kind=chr(data[p]);n=struct.unpack_from('<I',data,p+1)[0];p+=5
        if n>len(data)-p:raise ValueError('truncated record body')
        yield kind,data[p:p+n]
        p+=n

def angle(m):return math.atan2(m[4],m[0])
def diff(a,b):return math.atan2(math.sin(a-b),math.cos(a-b))

def check(root,enabled,recenter,motion=False):
    results=[];baseline=None
    for phase in [2,3,4,5,6,7]+([14,15] if recenter else [])+([16] if motion else []):
        path=root/f'phase-{phase}'/'fc2vr_cbcap.bin'
        codes={};events=list(records(path))
        motion_span=0
        if phase==16:
            angles=[]
            for kind,b in events:
                if kind=='X':
                    pose=struct.unpack('<96f',b);x,y,z,w=pose[53:57]
                    angles.append(math.atan2(2*(x*z+w*y),1-2*(x*x+y*y)))
            if len(angles)<3:raise ValueError('continuous-motion capture lacks frame poses')
            motion_span=max(angles)-min(angles)
            if motion_span<0.0001:raise ValueError('continuous-motion capture did not move the head')
        for kind,b in events:
            if kind=='C':codes[struct.unpack_from('<I',b)[0]]=hashlib.sha256(b[4:]).hexdigest()[:16]
        native={};actual={};eyes=collections.Counter();checks=0;max_error=0
        for kind,b in events:
            if kind in ('N','B'):
                addr,n=struct.unpack_from('<II',b)
                if n!=len(b)-8:raise ValueError('bad constant buffer length')
                (native if kind=='N' else actual)[addr]=b[8:]
            elif kind=='D':
                h=struct.unpack('<16I',b)
                if codes.get(h[6]) not in GRASS:continue
                eye=h[7]>>30
                if eye not in (0,1):continue
                addr=h[9]  # VS constant buffer slot 1
                if addr not in actual:raise ValueError('grass bound without captured upload')
                after=actual[addr]
                if len(after)!=1008:raise ValueError('unexpected grass provider layout')
                matrix=struct.unpack_from('<16f',after,512)
                if not all(math.isfinite(v) for v in matrix):raise ValueError('nonfinite grass matrix')
                if baseline is None:baseline=matrix
                target=angle(baseline)+(0 if enabled else math.radians(YAW[phase]))
                error=abs(diff(angle(matrix),target));max_error=max(max_error,error)
                if error>0.0001:raise ValueError(f'phase {phase}: grass facing follows headset ({error} rad error)')
                if enabled:
                    if addr not in native:raise ValueError('missing independent native upload')
                    before=native[addr]
                    if len(before)!=len(after):raise ValueError('native/actual upload size mismatch')
                    allowed={512+i for j in (0,1,4,5) for i in range(j*4,j*4+4)}
                    if any(x!=y and i not in allowed for i,(x,y) in enumerate(zip(before,after))):
                        raise ValueError('projection, roots, vertical scale or unrelated provider bytes changed')
                    # Other bound constant buffers include instance parameters,
                    # wind and material values; none may be modified.
                    for other in h[8:]:
                        if other!=addr and other in native and other in actual and native[other]!=actual[other]:
                            raise ValueError('unrelated bound constant buffer changed')
                checks+=1;eyes[eye]+=1
        if not checks or not eyes[0] or not eyes[1]:raise ValueError(f'phase {phase}: missing grass/stereo coverage')
        results.append(dict(phase=phase,observations=checks,eyes=dict(eyes),max_error_radians=max_error,head_motion_span_radians=motion_span))
    return results

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('capture_root',type=Path)
    parser.add_argument('--enabled',action='store_true')
    parser.add_argument('--recenter',action='store_true')
    parser.add_argument('--motion',action='store_true')
    args=parser.parse_args()
    if args.motion and not args.enabled:parser.error('--motion requires --enabled')
    result=check(args.capture_root,args.enabled,args.recenter,args.motion)
    (args.capture_root/'grass-assertions.json').write_text(json.dumps(result,indent=2))
    print(f'PASS: {sum(r["observations"] for r in result)} bound grass observations, both eyes, {len(result)} phases')
