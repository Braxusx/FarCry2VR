"""Verify native left-weapon checks reject wrong bones, action authority and gun drift."""
import argparse
from pathlib import Path
import re
from check_left_weapon_native import check,check_text

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('run',type=Path)
args=parser.parse_args()
source=(args.run/'native.log').read_text(errors='replace')
check(args.run/'native.log')
cases={
    'right hand holding weapon':source.replace('leftBone=56','leftBone=81'),
    'engine attachment overwritten':source.replace('attachment=56','attachment=81'),
    'visual fixture action authority':source.replace('authority=0,0','authority=1,0'),
    'rendered gun position drift':re.sub(r'positionError=[0-9.]+','positionError=0.5',source),
    'rendered gun rotation drift':re.sub(r'rotationError=[0-9.]+','rotationError=0.5',source),
    'back-of-hand wrist pose':re.sub(r'palmError=[0-9.]+','palmError=1.0',source),
    'camera-offset shot origin':re.sub(r'muzzleError=[0-9.]+','muzzleError=0.25',source),
    'wrong barrel direction':re.sub(r'barrelError=[0-9.]+','barrelError=0.5',source),
    'missing native muzzle':source.replace('muzzleReady=1','muzzleReady=0'),
    'wrong world/reference origin':re.sub(r'referenceError=[0-9.]+','referenceError=3000',source),
    'missing right eye':'\n'.join(line for line in source.splitlines() if not ('left-weapon-render' in line and 'eye=1' in line)),
}
for label,data in cases.items():
    try:
        check_text(data)
    except AssertionError:
        print(f'PASS: rejects {label}')
    else:
        raise AssertionError(f'Checker accepted {label}')
