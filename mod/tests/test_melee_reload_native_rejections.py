"""Reject unmirrored blades and RPG reload attachment drift in real native evidence."""
import argparse
from pathlib import Path
import re
from check_machete_native import check_text as blade_check
from check_reload_attachment_native import check_text as reload_check

parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path)
args=parser.parse_args();source=(args.run/'native.log').read_text(errors='replace')
blade_check(source);reload_check(source)
right={}
for line in source.splitlines():
    if 'melee-render sample=23' in line and 'left=0' in line:
        r=dict(re.findall(r'(\w+)=([^\s]+)',line));right[int(r['eye'])]=r['relative']
def change(prefix,transform):
    result=[]
    for line in source.splitlines():
        if prefix in line:
            r=dict(re.findall(r'(\w+)=([^\s]+)',line));value=transform(r)
            line=re.sub(r'relative=[^\s]+','relative='+value,line)
        result.append(line)
    return '\n'.join(result)
def displaced(r):
    matrix=list(map(float,r['relative'].split(',')));matrix[12]+=.415
    return ','.join(map(str,matrix))
cases=[
    ('unmirrored blade fallback',blade_check,change('melee-render',lambda r:right[int(r['eye'])] if r['left']=='1' else r['relative'])),
    ('blade outside palm',blade_check,change('melee-render sample=24',displaced)),
    ('wrong blade plane',blade_check,re.sub(r'normal=[^\s]+','normal=0,0,1',source)),
    ('wrong blade hand',blade_check,source.replace('left=1 key=570988e9 attachment=56','left=1 key=570988e9 attachment=81')),
    ('missing blade eye',blade_check,'\n'.join(line for line in source.splitlines() if not ('melee-render' in line and 'eye=1' in line))),
    ('animated RPG offset captured as rest',reload_check,change('reload-render sample=26',displaced)),
    ('RPG animation tail poisons rest',reload_check,change('reload-render sample=28',displaced)),
    ('RPG returns to native right hand',reload_check,source.replace('left=1 key=19a67e02 attachment=56','left=1 key=19a67e02 attachment=81')),
    ('replay gains input authority',reload_check,source.replace('authority=0,0','authority=1,0')),
    ('missing camera-parent replay',reload_check,'\n'.join(line for line in source.splitlines() if 'reload-attachment-replay phase=27' not in line)),
]
for name,checker,data in cases:
    assert data!=source, f'negative case did not alter evidence: {name}'
    try:checker(data)
    except AssertionError:print(f'PASS: rejects {name}')
    else:raise AssertionError(f'Checker accepted {name}')
