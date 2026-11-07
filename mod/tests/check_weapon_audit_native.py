"""Audit saved-slot weapon transforms; this does not simulate firing or prove impacts."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re
from check_machete_native import mirror


def records(text, marker):
    for line in text.splitlines():
        if marker in line:
            yield dict(re.findall(r'(\w+)=([^\s]+)', line))


def matrix(record, field):
    value = list(map(float, record[field].split(',')))
    assert len(value) == 16 and all(map(math.isfinite, value)), 'invalid matrix'
    return value


def check(text, angles=True, count=4, fingers=False, rpg_front=False):
    samples = defaultdict(list)
    for row in records(text, 'weapon-audit sample='):
        phase = int(row['sample']) // 100000
        assert row['authority'] == '0,0', 'fixture gained action authority'
        samples[phase, int(row['eye'])].append(row)
    report = []
    for slot in range(1, count+1):
        base = 30 + (slot - 1) * 5
        keys = set()
        comparisons = []
        for eye in [0, 1]:
            for stage in range(5):
                rows = samples[base + stage, eye]
                assert len(rows) >= 3, f'slot {slot} stage {stage} eye {eye}: missing samples'
                for row in rows[-2:]:
                    assert row['planeReady'] == '1', 'missing authored reflection plane'
                    left = stage not in [1, 3]
                    assert row['left'] == str(int(left)), 'hand switch failed'
                    assert row['attachment'] == ('56' if left else '81'), 'wrong anatomical mount'
                    assert row['two'] == str(int(stage in [2, 3])), 'incorrect grip phase'
                    keys.add(row['key'])
            for left_stage, right_stage in [(0, 1), (2, 3), (4, 1)]:
                right_row = samples[base + right_stage, eye][-1]
                right = matrix(right_row, 'relative')
                plane = matrix(right_row, 'plane')
                expected = mirror(right, plane[:3], plane[12:15])
                for row in samples[base + left_stage, eye][-2:]:
                    left = matrix(row, 'relative')
                    error = max(abs(a-b) for a, b in zip(expected, left))
                    assert error < .015, f'entry {slot} stage {left_stage} eye {eye}: placement mismatch {error}'
                    position = math.dist(expected[12:15], left[12:15])
                    dot = sum(expected[i*4+j]*left[i*4+j] for i in range(3) for j in range(3))
                    rotation = math.degrees(math.acos(max(-1, min(1, (dot-1)/2))))
                    comparisons.append(dict(stage=left_stage, eye=eye, matrixError=error,
                                            positionError=position, rotationDegrees=rotation))
        assert len(keys) == 1, f'slot {slot} changed asset during comparison: {keys}'
        assert '00000000' not in keys, 'unidentified weapon asset'
        report.append(dict(slot=slot, key=keys.pop(), comparisons=comparisons,
                           maximumMatrixError=max(r['matrixError'] for r in comparisons)))
    assert len({item['key'] for item in report})==count, 'audit entries reused a weapon model; verify command and inventory slot'
    previews = list(records(text, 'projectile-preview sample='))
    assert previews, 'missing native angle previews'
    for row in previews:
        assert row['authority'] == '0,0' and row['ok'] == '1'
        assert float(row['originError']) < .001, 'incorrect selected projectile origin'
        if angles:
            assert float(row['nativeAngleError']) < .001, 'native angle round trip failed'
            assert row['shooterIgnore'] == '1', 'native shooter exclusion unavailable'
        if row['left'] == '1' and row['behind'] == '1':
            assert row['hitAllowed'] == '0', 'behind-barrel hit retained'
    observed=list(records(text, 'finger-observed sample='))
    finger_samples=defaultdict(set)
    maximum_finger_error=0
    for row in observed:
        phase=int(row['sample'])//100000
        actual=matrix(row,'relative');native=matrix(row,'native')
        # Independent world-basis reflection of the native right digit, expressed
        # in the mirrored anatomical wrist basis. Do not reuse the C++ helper.
        expected=[value*(-1 if (i//4==2)!=(i%4==2) else 1) for i,value in enumerate(native)]
        error=max(abs(a-b) for a,b in zip(actual,expected))
        if fingers:
            assert error<.015, f'finger engine readback mismatch: {error}'
        assert row['authority']=='0,0'
        digit=int(row['digit'])
        assert 0<=digit<15
        expected_right=94+digit if digit<3 else 79+digit
        assert int(row['leftBone'])==57+digit and int(row['rightBone'])==expected_right, 'incorrect native digit pairing'
        finger_samples[phase,int(row['eye'])].add(digit)
        maximum_finger_error=max(maximum_finger_error,error)
    if fingers:
        for slot in range(count):
            for stage in [0,2,4]:
                for eye in [0,1]:
                    assert finger_samples[30+slot*5+stage,eye]==set(range(15)), f'missing finger readback entry {slot+1} stage {stage} eye {eye}'
    rpg_samples=defaultdict(list)
    if rpg_front:
        for row in records(text,'weapon-forward sample='):
            if row['key']=='19a67e02' and row['two']=='0':
                assert row['authority']=='0,0'
                rpg_samples[int(row['sample'])//100000,int(row['eye'])].append(row)
        expected_phases=[]
        for item in report:
            if item['key']=='19a67e02':expected_phases.extend(30+(item['slot']-1)*5+stage for stage in [0,1,4])
        assert expected_phases, 'RPG was not actually selected'
        for phase in expected_phases:
            for eye in [0,1]:
                assert len(rpg_samples[phase,eye])>=3, 'missing RPG forward observations'
                for row in rpg_samples[phase,eye][-2:]:
                    direction=list(map(float,row['direction'].split(',')))
                    assert len(direction)==3 and all(map(math.isfinite,direction))
                    assert row['muzzle']=='3' and direction[2]<-.9, 'RPG used its rear/backblast locator'
                plane=matrix(samples[phase,eye][-1],'plane')
                assert plane[12]>.2 and plane[4]>.9, 'RPG launch locator is not in front of its grip'
    return dict(slots=report, projectilePreviews=len(previews), spawnedProjectiles=0,
                nativeAnglesChecked=angles,fingerReadbacks=len(observed),maximumFingerError=maximum_finger_error,
                fingerReadbacksPassed=bool(observed) and maximum_finger_error<.015,fingerReadbacksRequired=fingers,
                rpgFrontRequired=rpg_front)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--baseline-observer', action='store_true', help='report old capture with known angle observer offset bug')
    parser.add_argument('--count',type=int,default=4)
    parser.add_argument('--fingers',action='store_true')
    parser.add_argument('--rpg-front',action='store_true')
    args = parser.parse_args()
    assert 1<=args.count<=29
    result = check((args.run/'native.log').read_text(errors='replace'), not args.baseline_observer,args.count,args.fingers,args.rpg_front)
    (args.run/'weapon-audit-report.json').write_text(json.dumps(result, indent=2)+'\n')
    for slot in result['slots']:
        print(f"slot {slot['slot']} key {slot['key']}: maximum matrix difference {slot['maximumMatrixError']:.6f}")
    print(f"{result['projectilePreviews']} native projectile previews; no projectiles spawned")
