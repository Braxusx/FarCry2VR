"""Negative checks using a real passing capture; files are never modified."""
import argparse
import struct
import check_grass_gpu as validator

def run(root):
    original=validator.records
    for case,expected in [('unfixed','grass facing follows headset'),('projection','unrelated provider bytes changed'),('eye','missing grass/stereo coverage')]:
        def altered(path):
            native={}
            for kind,b in original(path):
                if kind=='N':native[struct.unpack_from('<I',b)[0]]=b[8:]
                if kind=='D' and case=='eye' and struct.unpack_from('<I',b,28)[0]>>30==1:continue
                if kind=='B' and path.parent.name=='phase-3':
                    addr,n=struct.unpack_from('<II',b)
                    if n==1008 and addr in native:
                        data=bytearray(b)
                        if case=='unfixed':data[520:584]=native[addr][512:576]
                        elif case=='projection':data[8]^=1
                        b=bytes(data)
                yield kind,b
        validator.records=altered
        try:
            validator.check(root,True,True)
            raise AssertionError(f'{case} was accepted')
        except ValueError as error:
            if expected not in str(error):raise
            print(f'PASS: rejects {case}: {error}')
        finally:validator.records=original

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('capture_root',type=validator.Path)
    run(parser.parse_args().capture_root)
