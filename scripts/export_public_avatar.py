"""Generate an original abstract XYVG audio indicator, without reference artwork.

The five polygons match the existing firmware renderer's fixed format. No browser,
private assets, API calls or hardware access are required. Run before flash.ps1.
"""
from pathlib import Path
import math
import struct

ROOT = Path(__file__).resolve().parents[1]
FRAMES = 384
COUNTS = (32, 32, 128, 64, 64)

def ellipse(cx, cy, rx, ry, n):
    return [(round((cx+rx*math.cos(2*math.pi*i/n))*16),
             round((cy+ry*math.sin(2*math.pi*i/n))*16)) for i in range(n)]

def main():
    output=ROOT/'.local/avatar-vector.bin';output.parent.mkdir(parents=True,exist_ok=True)
    color=((243>>3)<<11)|((241>>2)<<5)|(235>>3)
    background=struct.pack('<HHHH',65535,color,360*360-65535,color)
    header=b'XYVG'+struct.pack('<8I',360,360,6,FRAMES,sum(COUNTS),sum(COUNTS)*4,len(background),36+len(background))
    frames=bytearray()
    for bank in range(6):
        for frame in range(FRAMES):
            phase=2*math.pi*frame/FRAMES
            amp=(.04,.16,.12,.05,.35,.7)[bank]
            beat=amp*(.65+.35*math.sin(phase*8))
            parts=(ellipse(58,180,7,7,32),ellipse(302,180,7,7,32),
                   ellipse(180,180,88+beat*3,88+beat*3,128),
                   ellipse(162,180,6,20+beat*42,64),ellipse(198,180,6,20+beat*30,64))
            values=[v for part in parts for point in part for v in point]
            frames.extend(struct.pack('<640h',*values))
    output.write_bytes(header+background+frames)
    print(f'Wrote public placeholder: {output} ({output.stat().st_size} bytes)')

if __name__=='__main__': main()
