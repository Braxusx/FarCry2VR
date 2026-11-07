"""Reject missing/wrong F1 transitions, stale input and mirrored right attachments."""
import argparse
from pathlib import Path
import re
from check_hand_switch_native import check_text

parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path)
args=parser.parse_args();source=(args.run/'native.log').read_text(errors='replace')
check_text(source)
event=next(line for line in source.splitlines() if 'weapon-hand changed left=' in line)
cases={
    'held F1 repeats':source+'\n'+event,
    'old input lease still current':source.replace('inputSynced=0','inputSynced=1'),
    'input generation not advanced':re.sub(r'inputGeneration=\d+','inputGeneration=1',source),
    'old layout arm frame':re.sub(r'(hand-switch-rig[^\n]*layout=)\d+',r'\g<1>1',source),
    'right hand retains left attachment':source.replace('left=0 attachment=81','left=0 attachment=56'),
    'right hand retains mirrored offset':re.sub(r'nativeOffsetError=[0-9.]+','nativeOffsetError=1.0',source),
    'wrong physical wrist target':re.sub(r'rightError=[0-9.]+','rightError=0.5',source),
    'synthetic hand acquired authority':source.replace('authority=0,0','authority=1,0'),
    'missing right-mode eye':'\n'.join(line for line in source.splitlines() if not ('hand-switch-render' in line and 'eye=1' in line)),
    'F1 changed body reference':'\n'.join(re.sub(r'reference=\d+','reference=999',line) if ('body-yaw-eye' in line and 'sample=18' in line) else line for line in source.splitlines()),
}
for label,data in cases.items():
    assert data!=source, f'negative case did not change evidence: {label}'
    try:check_text(data)
    except AssertionError:print(f'PASS: rejects {label}')
    else:raise AssertionError(f'Checker accepted {label}')
