"""Require the weapon audit to reject corrupted native evidence."""
import argparse
from pathlib import Path
import re
from check_weapon_audit_native import check

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('run',type=Path)
parser.add_argument('--count',type=int,default=4)
parser.add_argument('--fingers',action='store_true')
parser.add_argument('--rpg-front',action='store_true')
args=parser.parse_args()
text=(args.run/'native.log').read_text(errors='replace')
check(text,count=args.count,fingers=args.fingers,rpg_front=args.rpg_front)

def reject(label,change):
    changed=change(text)
    assert changed!=text, f'{label}: mutation did not affect trace'
    try:
        check(changed,count=args.count,fingers=args.fingers,rpg_front=args.rpg_front)
    except (AssertionError,ValueError,KeyError):
        print('PASS: rejects '+label)
        return
    raise AssertionError('accepted '+label)

reject('missing second eye',lambda s:'\n'.join(line for line in s.splitlines() if not ('weapon-audit sample=' in line and 'eye=1 ' in line)))
reject('synthetic input authority',lambda s:s.replace('authority=0,0','authority=1,1'))
reject('wrong anatomical mount',lambda s:s.replace('attachment=56','attachment=81'))
reject('missing authored plane',lambda s:s.replace('planeReady=1','planeReady=0'))
reject('wrong hand transition',lambda s:re.sub(r'(weapon-audit .*?)left=0',r'\1left=1',s))
reject('missing support-grip phase',lambda s:re.sub(r'(weapon-audit .*?)two=1',r'\1two=0',s))
reject('projectile origin regression',lambda s:re.sub(r'(projectile-preview .*?)originError=[^ ]+',r'\1originError=0.75',s))
reject('native angle inversion',lambda s:re.sub(r'(projectile-preview .*?)nativeAngleError=[^ ]+',r'\1nativeAngleError=2',s))
reject('missing shooter exclusion',lambda s:s.replace('shooterIgnore=1','shooterIgnore=0'))
reject('behind-barrel forced hit',lambda s:re.sub(r'(projectile-preview .*?left=1 behind=1 .*?)hitAllowed=0',r'\1hitAllowed=1',s))

def drift(s):
    output=[]
    for line in s.splitlines():
        if 'weapon-audit sample=' in line and ' left=1 ' in line:
            original=re.search(r'relative=([^ ]+)',line).group(1)
            values=list(map(float,original.split(',')));values[12]+=.05
            line=line.replace('relative='+original,'relative='+','.join(map(str,values)))
        output.append(line)
    return '\n'.join(output)

reject('weapon placement drift',drift)
if args.count>1:
    reject('duplicate weapon selection',lambda s:re.sub(r'(weapon-audit .*?)key=[0-9a-f]+',r'\1key=19a67e02',s))
if args.fingers:
    reject('missing native finger readback',lambda s:'\n'.join(line for line in s.splitlines() if 'finger-observed sample=' not in line))
    reject('thumb/index mispairing',lambda s:s.replace('rightBone=94','rightBone=82'))
if args.rpg_front:
    reject('RPG rear muzzle selection',lambda s:re.sub(r'(weapon-forward .*?key=19a67e02 .*?)muzzle=3',r'\1muzzle=2',s))
    reject('missing RPG forward evidence',lambda s:'\n'.join(line for line in s.splitlines() if not ('weapon-forward sample=' in line and 'key=19a67e02' in line)))
