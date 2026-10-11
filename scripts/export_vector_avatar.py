"""Sample the existing character's actual SVG geometry for one-shape morphing.

XYVG v1: magic; 8 uint32 (width, height, banks, frames, points, stride,
background_size, data_offset); RGB565 RLE background; fixed Q4 xy vertices.
Parts, in painter order: 2 wave beads (32 points each), body (128), eyes (64 each).
"""
from pathlib import Path
import io
import json
import struct
import numpy as np
from PIL import Image, ImageDraw
from playwright.sync_api import sync_playwright

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.local/vector-avatar'
SOURCE=ROOT.parent/'xiaoyun-realtime/web/vendor/xiaoyun-engine.js'
BANKS=[('idle',0),('listening',0),('curious',0),('dictating',0),('dictating',.35),('dictating',.85)]
COUNTS=[32,32,128,64,64]
FRAMES=384
COLOR='#D97527'

def rle(png):
    pixels=np.array(Image.open(io.BytesIO(png)).convert('RGB'),dtype=np.uint16)
    values=((pixels[:,:,0]>>3)<<11 | (pixels[:,:,1]>>2)<<5 | pixels[:,:,2]>>3).ravel()
    out=bytearray();value=int(values[0]);count=0
    for v in values:
        if v==value and count<65535:count+=1
        else:out.extend(struct.pack('<HH',count,value));value=int(v);count=1
    out.extend(struct.pack('<HH',count,value));return out

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    source=SOURCE.read_text(encoding='utf-8').replace('export { GrokCharacter };','').replace('export const GrokCharacter =','window.GrokCharacter =').replace('export { grokScope };','')
    banks=[]
    with sync_playwright() as p:
        browser=p.chromium.launch(headless=True,channel='msedge')
        page=browser.new_page(viewport={'width':360,'height':360},device_scale_factor=1)
        page.set_content('''<style>html,body{margin:0;width:360px;height:360px;background:#f3f1eb;overflow:hidden}svg{position:absolute;left:42px;top:17px;width:276px;height:276px}.title{position:absolute;top:301px;width:100%;text-align:center;font:600 23px "Microsoft YaHei";color:#232b30;letter-spacing:4px}</style><svg id="avatar" xmlns="http://www.w3.org/2000/svg"></svg><div class="title">小萌</div>''')
        background=page.screenshot();(OUT/'background.png').write_bytes(background)
        page.evaluate('''()=>{window.simTime=1000;window.seed=4;Math.random=()=>((window.seed=(Math.imul(window.seed,1664525)+1013904223)>>>0)/4294967296);Object.defineProperty(performance,'now',{value:()=>simTime});requestAnimationFrame=()=>1;cancelAnimationFrame=()=>{};}''')
        page.add_script_tag(content=source)
        for bank,(state,level) in enumerate(BANKS):
            page.evaluate('''({state,level,color})=>{if(window.bot)bot.destroy();document.querySelector('#avatar').innerHTML='';simTime=1000;seed=4;window.bot=new GrokCharacter(document.querySelector('#avatar'),{mode:'hold',state,shape:'blob',color:'black',loginWrap:true,inkFlat:color,eyeColor:'#fbffff',followPointer:false,paused:false,ambientTricks:false,reduceMotion:false,faceOverlays:state==='dictating',keepWaveFace:true,overlayTurns:false});bot.audioLevel=level;for(let i=0;i<90;i++){simTime+=1000/60;bot._tick(simTime);}}''',{'state':state,'level':level,'color':COLOR})
            frames=[]
            # Batch sampling avoids browser round trips for individual vertices.
            for first in range(0,FRAMES,16):
                frames.extend(page.evaluate('''()=>{let frames=[];for(let frame=0;frame<16;frame++){
                    for(let i=0;i<2;i++){simTime+=1000/60;bot._tick(simTime);}
                    // Left wave beads are paths, right beads are circles. The
                    // inner pair is occluded by the central face; retain the outer pair.
                    let dots=[bot.fx.dots[0],bot.fx.parts[6]].map(e=>e&&getComputedStyle(e).display!=='none'?e:null);
                    let parts=[dots[0],dots[1],bot.body,...bot.eyeEls], counts=[32,32,128,64,64], xy=[];
                    for(let part=0;part<parts.length;part++){
                        let e=parts[part];
                        if(!e){for(let i=0;i<counts[part];i++)xy.push(180*16,150*16);continue;}
                        let length=e.getTotalLength(),matrix=e.getScreenCTM();
                        for(let i=0;i<counts[part];i++){
                            let pt=e.getPointAtLength(length*i/counts[part]);let q=new DOMPoint(pt.x,pt.y).matrixTransform(matrix);
                            xy.push(Math.round(q.x*16),Math.round(q.y*16));
                        }
                    }
                    frames.push(xy);
                }return frames;}'''))
                if first==0:page.screenshot(path=str(OUT/f'source-{bank}.png'))
            banks.append(np.asarray(frames,dtype='<i2'))
            print(f'Geometry bank {bank}: {state} / {level}',flush=True)
        browser.close()
    bg=rle(background);points=sum(COUNTS);offset=36+len(bg)
    header=b'XYVG'+struct.pack('<8I',360,360,len(BANKS),FRAMES,points,points*4,len(bg),offset)
    payload=np.asarray(banks,dtype='<i2').tobytes()
    target=ROOT/'.local/avatar-vector.bin';target.write_bytes(header+bg+payload)
    assert target.stat().st_size<0x970000
    np.save(OUT/'geometry.npy',np.asarray(banks))
    (OUT/'manifest.json').write_text(json.dumps({'source':str(SOURCE),'name':'小萌','color':COLOR,'banks':BANKS,'frames':FRAMES,'fps':30,'part_points':COUNTS,'bytes':target.stat().st_size},ensure_ascii=False,indent=2),encoding='utf-8')
    print(f'{target}: {target.stat().st_size} bytes',flush=True)

if __name__=='__main__':main()
