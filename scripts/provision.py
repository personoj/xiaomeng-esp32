"""Provision USB or Wi-Fi mode without printing credentials."""
import argparse,json,time,urllib.request
from pathlib import Path
import serial

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--mode',choices=['usb','wifi'],required=True)
p.add_argument('--file',type=Path,default=ROOT/'.local/network.json');p.add_argument('--port',default='COM9')
a=p.parse_args()
message={'type':'device.configure','mode':a.mode}
if a.mode=='wifi':
    config=json.loads(a.file.read_text(encoding='utf-8-sig'))
    pair=json.loads((ROOT.parent/'xiaoyun-realtime/.local/device-pairing.json').read_text(encoding='utf-8'))
    server=config['server'];ssid=config['ssid'];password=config.get('password','')
    if len(ssid.encode())>32 or len(password.encode())>64: raise ValueError('SSID/password too long')
    if '/' in server or ':' not in server: raise ValueError('server must be IPv4:port')
    request=urllib.request.Request('http://'+server+'/device/health',headers={'Authorization':'Bearer '+pair['token']})
    with urllib.request.urlopen(request,timeout=5) as r:
        if json.load(r)['session_id']!=pair['session_id']: raise ValueError('Pairing mismatch')
    message.update(ssid=ssid,password=password,server=server,token=pair['token'])
s=serial.Serial(port=None,baudrate=921600,timeout=.1,write_timeout=3)
s.dtr=False;s.rts=False;s.port=a.port;s.open()
s.write((json.dumps(message,separators=(',',':'))+'\n').encode())
end=time.monotonic()+4
while time.monotonic()<end:
    raw=s.readline()
    try: e=json.loads(raw)
    except ValueError: continue
    if e.get('kind')=='configured': print('Configuration saved:',a.mode);break
else: raise RuntimeError('Device did not acknowledge configuration')
s.close()
