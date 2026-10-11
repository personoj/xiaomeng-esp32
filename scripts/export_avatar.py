"""Sample the existing JS character engine into lossless RGB565 RLE sprites.

The source engine remains authoritative; no replacement artwork is generated.
File header: XYAV, uint32 width,height,banks,frames; then offset/size pairs.
Runs: uint16 count, uint16 RGB565 (little endian). Device swaps for the panel.
"""
from pathlib import Path
import struct
import io
import json
from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.local' / 'avatar'
OUT.mkdir(parents=True, exist_ok=True)
SOURCE = ROOT.parent / 'xiaoyun-realtime' / 'web' / 'vendor' / 'xiaoyun-engine.js'
BANKS = [('idle', 0), ('listening', 0), ('curious', 0), ('dictating', 0), ('dictating', .35), ('dictating', .85)]
FRAMES = 192
source = SOURCE.read_text(encoding='utf-8')
# Keep module-local geometry/engine identical and expose only the constructor.
source = source.replace('export { GrokCharacter };', '')
source = source.replace('export const GrokCharacter =', 'window.GrokCharacter =')
source = source.replace('export { grokScope };', '')
if 'export ' in source:
    print('Remaining exports:', [line for line in source.splitlines() if line.startswith('export ')])

def rle(png):
    im = Image.open(io.BytesIO(png)).convert('RGB')
    values = [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r,g,b in im.getdata()]
    out = bytearray()
    prev, count = values[0], 0
    for v in values:
        if v == prev and count < 65535:
            count += 1
        else:
            out.extend(struct.pack('<HH', count, prev))
            prev, count = v, 1
    out.extend(struct.pack('<HH', count, prev))
    return out

all_frames = []
with sync_playwright() as p:
    browser = p.chromium.launch(headless=True, channel='msedge')
    page = browser.new_page(viewport={'width':360, 'height':360}, device_scale_factor=1)
    page.set_content('''<style>html,body{margin:0;width:360px;height:360px;background:#f3f1eb;overflow:hidden}svg{position:absolute;left:42px;top:17px;width:276px;height:276px} .title{position:absolute;top:301px;width:100%;text-align:center;font:600 23px "Microsoft YaHei";color:#232b30;letter-spacing:4px}</style><svg id="avatar" xmlns="http://www.w3.org/2000/svg"></svg><div class="title">小云</div>''')
    page.evaluate('''() => { window.simTime=1000; window.seed=4; Math.random=()=>((window.seed=(Math.imul(window.seed,1664525)+1013904223)>>>0)/4294967296); Object.defineProperty(performance,'now',{value:()=>window.simTime}); window.requestAnimationFrame=()=>1;window.cancelAnimationFrame=()=>{}; }''')
    page.add_script_tag(content=source)
    for bank, (state, level) in enumerate(BANKS):
        page.evaluate('''({state,level}) => {
            if(window.bot)window.bot.destroy(); document.querySelector('#avatar').innerHTML='';simTime=1000;window.seed=4;
            window.bot=new window.GrokCharacter(document.querySelector('#avatar'),{mode:'hold',state,shape:'blob',color:'black',loginWrap:true,inkFlat:'#232b30',eyeColor:'#fbffff',followPointer:false,paused:false,ambientTricks:false,reduceMotion:false,faceOverlays:state==='dictating',keepWaveFace:true,overlayTurns:false});
            bot.audioLevel=level;for(let i=0;i<90;i++){simTime+=1000/60;bot._tick(simTime);}
        }''', {'state':state,'level':level})
        for frame in range(FRAMES):
            page.evaluate('''() => {for(let i=0;i<2;i++){simTime+=1000/60;bot._tick(simTime);}}''')
            png = page.screenshot()
            if frame == 12:
                (OUT / f'{bank}-{state}.png').write_bytes(png)
            all_frames.append(rle(png))
        print(f'Bank {bank}: {state} level={level}', flush=True)
    browser.close()

head = bytearray(b'XYAV' + struct.pack('<IIII',360,360,len(BANKS),FRAMES))
offset = len(head) + len(all_frames)*8
for frame in all_frames:
    head.extend(struct.pack('<II', offset, len(frame)))
    offset += len(frame)
target = ROOT / '.local' / 'avatar.bin'
target.write_bytes(head + b''.join(all_frames))
if target.stat().st_size > 0x970000:
    raise RuntimeError('Avatar exceeds partition')
(OUT / 'manifest.json').write_text(json.dumps({'source':str(SOURCE),'frames':FRAMES,'fps':30,'banks':BANKS,'bytes':target.stat().st_size},ensure_ascii=False,indent=2),encoding='utf-8')
print(f'{target}: {target.stat().st_size} bytes')
