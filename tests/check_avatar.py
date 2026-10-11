"""Render actual firmware C code; compare source SVG frames and transition frames."""
import ctypes as c
import io
import json
from pathlib import Path
import struct
import sys
import numpy as np
from PIL import Image,ImageDraw

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.local/animation-review';OUT.mkdir(exist_ok=True)
lib=c.CDLL(sys.argv[1])
lib.avatar_renderer_size.restype=c.c_size_t
lib.avatar_init.argtypes=[c.c_void_p,c.c_void_p,c.c_size_t,c.c_void_p];lib.avatar_init.restype=c.c_bool
lib.avatar_render.argtypes=[c.c_void_p,c.c_void_p,c.c_uint32,c.c_uint,c.c_uint]
lib.avatar_overflows.argtypes=[c.c_void_p];lib.avatar_overflows.restype=c.c_uint
asset=(ROOT/'.local/avatar-vector.bin').read_bytes();data=c.create_string_buffer(asset)
N=360*360

class Renderer:
    def __init__(self):
        self.ctx=c.create_string_buffer(lib.avatar_renderer_size())
        self.bg=(c.c_uint16*N)();self.guarded=(c.c_uint16*(N+16))()
        for i in list(range(8))+list(range(N+8,N+16)):self.guarded[i]=0xdead
        self.out=c.byref(self.guarded,16)
        assert lib.avatar_init(self.ctx,data,len(asset),self.bg)
    def render(self,ms,pose,level=0):
        lib.avatar_render(self.ctx,self.out,ms,pose,level)
        assert lib.avatar_overflows(self.ctx)==0
        assert all(self.guarded[i]==0xdead for i in list(range(8))+list(range(N+8,N+16)))
        pixels=np.ctypeslib.as_array(self.guarded)[8:N+8].byteswap().reshape(360,360).copy()
        return pixels

def rgb(pixels):
    return np.stack([(pixels>>11)*255//31,((pixels>>5)&63)*255//63,(pixels&31)*255//31],axis=-1).astype('uint8')

def sheet(images,labels,path,columns=6):
    tile=200;canvas=Image.new('RGB',(columns*tile,((len(images)+columns-1)//columns)*(tile+24)), '#ffffff');draw=ImageDraw.Draw(canvas)
    for i,(im,label) in enumerate(zip(images,labels)):
        x=(i%columns)*tile;y=(i//columns)*(tile+24)
        canvas.paste(im.resize((tile,tile)),(x,y));draw.text((x+5,y+tile+3),label,fill='#303030')
    canvas.save(path)

report={'source_mae':[],'frames_checked':0,'max_intermediate_pixels':0}
for bank,level in enumerate([0,0,0,0,358,870]):
    pose=min(bank,3);r=Renderer();actual=rgb(r.render(500,pose,level))
    source=np.array(Image.open(ROOT/f'.local/vector-avatar/source-{bank}.png').convert('RGB'))
    error=float(np.mean(np.abs(actual.astype(float)-source)))
    report['source_mae'].append(round(error,3));assert error<4,(bank,error)
    if bank>=3:
        # Global MAE can hide a missing bead; check both outer bead regions.
        for x0,x1 in [(45,99),(262,315)]:
            roi_error=float(np.mean(np.abs(actual[100:205,x0:x1].astype(float)-source[100:205,x0:x1])))
            assert roi_error<7,(bank,'missing outer bead',x0,roi_error)
    Image.fromarray(actual).save(OUT/f'pose-{bank}.png')
    r=Renderer()
    for frame in range(384):
        pixels=r.render(round(frame*1000/30),pose,level);report['frames_checked']+=1

r=Renderer();frames=[];labels=[];animation=[]
palette=[0xdba4,0xffff,int(np.ctypeslib.as_array(r.bg)[0].byteswap())]
for frame in range(240):
    ms=round(frame*1000/30)
    pose=0 if ms<1000 else 1 if ms<2400 else 2 if ms<3600 else 3 if ms<5800 else 0
    level=int((.45+.4*np.sin(ms/170))*1024) if pose==3 else 0
    pixels=r.render(ms,pose,level);report['frames_checked']+=1
    intermediate=int(np.count_nonzero(~np.isin(pixels[:295],palette)))
    report['max_intermediate_pixels']=max(report['max_intermediate_pixels'],intermediate)
    assert intermediate<2400,(ms,intermediate)
    im=Image.fromarray(rgb(pixels));animation.append(im.resize((252,252)))
    if frame in [0,27,30,32,35,40,70,72,74,77,81,89,105,108,110,113,117,125,170,174,176,179,183,191]:
        frames.append(im);labels.append(f'{ms} ms / state {pose}')
sheet(frames,labels,OUT/'after-transitions.png')
animation[0].save(OUT/'xiaomeng-transitions.gif',save_all=True,append_images=animation[1:],duration=33,loop=0)

old=(ROOT/'.local/avatar-raster-v0.2.bin').read_bytes()
def decode(index):
    offset,size=struct.unpack_from('<II',old,20+index*8)
    runs=np.frombuffer(old[offset:offset+size],dtype='<u2').reshape(-1,2)
    return rgb(np.repeat(runs[:,1],runs[:,0]).reshape(360,360))
a=decode(192+72).astype(float);images=[]
for i in range(8):
    b=decode(5*192+i).astype(float);images.append(Image.fromarray(np.rint(a*(1-(i+1)/8)+b*(i+1)/8).astype('uint8')))
sheet(images,[f'old fade {i+1}/8' for i in range(8)],OUT/'before-ghosting.png',4)
(OUT/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report),flush=True)
