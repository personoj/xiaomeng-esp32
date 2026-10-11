"""Private USB JSON/PCM bridge to the existing authenticated 8766 audio channel."""
import argparse
import asyncio
import base64
import json
import secrets
import time
import urllib.request
from pathlib import Path
import serial
import websockets

ROOT = Path(__file__).resolve().parents[1]
PAIR = ROOT.parent/'xiaoyun-realtime'/'.local/device-pairing.json'


def pairing(server):
    if PAIR.exists():
        return json.loads(PAIR.read_text(encoding='utf-8'))
    request=urllib.request.Request(server+'/api/sessions',data=b'{}',headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(request,timeout=5) as r: sid=json.load(r)['session_id']
    value={'session_id':sid,'token':secrets.token_urlsafe(48)}
    PAIR.parent.mkdir(exist_ok=True)
    PAIR.write_text(json.dumps(value,indent=2),encoding='utf-8')
    return value


async def run(args):
    pair=pairing(args.server)
    log_path=ROOT/'.local/bridge-events.jsonl'
    log_file=None
    def log(event):
        row={'at':time.time(),**event}
        line=json.dumps(row,ensure_ascii=False)
        log_file.write(line+'\n');log_file.flush()
        print(line,flush=True)
    # Keep an idle read from holding the first downlink ACK for up to 150 ms.
    ser=serial.Serial(port=None,baudrate=921600,timeout=.02,write_timeout=2)
    ser.dtr=False;ser.rts=False;ser.port=args.port;ser.open()
    log_file=log_path.open('a',encoding='utf-8')
    write_lock=asyncio.Lock()
    ws=None;reader=None;epoch=0;pending_text=args.test_text
    stats={'input_samples':0,'output_samples':0,'played_samples':0,'drained':0}
    ready=asyncio.Event()
    audio_ack={}
    speech_stopped_ms=None
    input_end_source='vad'

    async def send_device(e):
        e={'device_epoch':epoch,**e}
        packet=(json.dumps(e,ensure_ascii=False,separators=(',',':'))+'\n').encode()
        async with write_lock:
            await asyncio.to_thread(ser.write,packet)

    async def model_reader(socket):
        nonlocal pending_text
        sequence={}
        seen={}
        try:
            async for raw in socket:
                e=json.loads(raw);kind=e.get('type')
                if kind=='attached':
                    await socket.send(json.dumps({'type':'session.start','input_mode':'push_to_talk'}))
                elif kind=='audio.delta':
                    rid=e['response_id'];original=e['seq']
                    if original<=seen.get(rid,0): continue
                    seen[rid]=original
                    pcm=base64.b64decode(e['audio'])
                    stats['output_samples']+=len(pcm)//2
                    for start in range(0,len(pcm),3840):
                        sequence[rid]=sequence.get(rid,0)+1
                        key=(rid,sequence[rid]);ack=audio_ack[key]=asyncio.Event()
                        sent_at=time.monotonic()
                        await send_device({**e,'seq':sequence[rid],'audio':base64.b64encode(pcm[start:start+3840]).decode()})
                        try:
                            await asyncio.wait_for(ack.wait(),2)
                            if sequence[rid]==1:
                                log({'type':'bridge.first_audio_ack','response_id':rid,'delivery_ms':round((time.monotonic()-sent_at)*1000)})
                        finally: audio_ack.pop(key,None)
                    continue
                elif kind=='ready':
                    ready.set()
                await send_device(e)
                if kind not in ('transcript.delta','integration.updated'):
                    log({k:v for k,v in e.items() if k not in ('connection_id','audio')})
                if kind=='ready' and pending_text:
                    text,pending_text=pending_text,None
                    await socket.send(json.dumps({'type':'input.text','text':text},ensure_ascii=False))
        except asyncio.CancelledError: raise
        except Exception as exc:
            log({'type':'bridge.error','error':type(exc).__name__})
            await send_device({'type':'device.standby','reason':'bridge_disconnected'})

    async def disconnect():
        nonlocal ws,reader
        ready.clear()
        if reader:
            reader.cancel();await asyncio.gather(reader,return_exceptions=True);reader=None
        if ws:
            try: await ws.send(json.dumps({'type':'session.stop'}))
            except Exception: pass
            await ws.close();ws=None

    async def connect():
        nonlocal ws,reader
        await disconnect()
        url=args.server.replace('http://','ws://').replace('https://','wss://')+'/device/ws'
        ws=await websockets.connect(url,additional_headers={'Authorization':'Bearer '+pair['token']},max_size=2**20)
        reader=asyncio.create_task(model_reader(ws))

    log({'type':'bridge.ready','port':args.port,'session_id':pair['session_id'],'server':args.server})
    deadline=time.monotonic()+args.duration if args.duration else float('inf')
    startup=time.monotonic();wake_sent=False;line=bytearray();heartbeat=0
    command_file=ROOT/'.local/bridge-command.json'
    try:
        while time.monotonic()<deadline:
            if time.monotonic()-heartbeat>1:
                await send_device({'type':'device.heartbeat'});heartbeat=time.monotonic()
            if args.test_text and not wake_sent and time.monotonic()-startup>2:
                await send_device({'type':'device.wake'});wake_sent=True
            if command_file.exists():
                command=json.loads(command_file.read_text(encoding='utf-8-sig'));command_file.unlink()
                if command.get('shutdown'): break
                if command.get('text'):
                    if ws and ready.is_set():
                        await ws.send(json.dumps({'type':'input.text','text':command['text']},ensure_ascii=False))
                    else: pending_text=command['text']
                if command.get('wake'): await send_device({'type':'device.wake'})
                if command.get('sleep'): await send_device({'type':'device.standby'})
            block=await asyncio.to_thread(ser.read, min(max(ser.in_waiting,1),8192))
            line.extend(block)
            while b'\n' in line:
                raw,_,rest=line.partition(b'\n');line=bytearray(rest)
                try: e=json.loads(raw)
                except (ValueError,UnicodeDecodeError): continue
                kind=e.get('type')
                if kind=='device.status' and e.get('kind')=='boot':
                    await disconnect();epoch=e.get('device_epoch',0);speech_stopped_ms=None
                elif kind=='device.telemetry' and ws is None:
                    epoch=e.get('device_epoch',epoch)
                    if e.get('state',0)!=0:await send_device({'type':'device.standby'})
                elif kind=='device.wake':
                    epoch=e.get('device_epoch',epoch+1)
                    speech_stopped_ms=None
                    try: await connect()
                    except Exception as exc:
                        log({'type':'bridge.connect_error','error':type(exc).__name__})
                        await send_device({'type':'device.standby'})
                elif kind=='device.sleep':
                    speech_stopped_ms=None
                    await disconnect()
                elif kind=='device.speech':
                    speech_stopped_ms=e.get('uptime_ms') if e.get('state')=='stopped' else None
                    input_end_source='vad'
                elif kind=='device.capture':
                    if e.get('state')=='released':
                        speech_stopped_ms=e.get('uptime_ms');input_end_source='button_release'
                    if e.get('state')=='pressed':speech_stopped_ms=None
                elif kind=='device.playback_started':
                    if speech_stopped_ms is not None:
                        elapsed=e['uptime_ms']-speech_stopped_ms
                        if elapsed>=0:
                            metric='button_release_to_play_ms' if input_end_source=='button_release' else 'local_vad_end_to_play_ms'
                            log({'type':'bridge.speech_to_playback','response_id':e['response_id'],metric:elapsed})
                    speech_stopped_ms=None
                elif kind=='device.audio_ack':
                    if ack:=audio_ack.get((e.get('response_id'),e.get('seq'))): ack.set()
                elif kind in ('input.begin','input.audio','input.commit','playback.progress','interrupt','session.stop','device.standby.ack'):
                    if kind=='input.audio':
                        stats['input_samples']+=len(base64.b64decode(e['audio']))//2
                    elif kind=='playback.progress':
                        stats['played_samples']=e['played_samples']
                        stats['drained']+=bool(e.get('drained'))
                    if ws:
                        try: await ws.send(json.dumps(e,separators=(',',':')))
                        except websockets.ConnectionClosed: await disconnect()
                    elif kind=='input.audio':
                        epoch=e.get('device_epoch',epoch)
                        await send_device({'type':'device.standby'})
                if kind not in ('input.audio','device.audio_ack'): log(e)
            if len(line)>131072: line.clear()
    finally:
        try:
            await send_device({'type':'device.standby'})
            await asyncio.sleep(.15)
        except (serial.SerialException,OSError):pass
        await disconnect();ser.close()
        log({'type':'bridge.summary',**stats});log_file.close()


if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--port',default='COM9');p.add_argument('--server',default='http://127.0.0.1:8766')
    p.add_argument('--test-text');p.add_argument('--duration',type=float,default=0)
    p.add_argument('--wait-for-usb',action='store_true')
    args=p.parse_args();waiting_reported=False
    while True:
        try:
            asyncio.run(run(args));break
        except serial.SerialException as exc:
            if not args.wait_for_usb:raise
            if not waiting_reported:
                print(json.dumps({'type':'bridge.waiting_for_usb','port':args.port,'error':type(exc).__name__}),flush=True)
                waiting_reported=True
            command=ROOT/'.local/bridge-command.json'
            if command.exists():
                try: shutdown=json.loads(command.read_text(encoding='utf-8-sig')).get('shutdown')
                except ValueError: shutdown=False
                if shutdown:command.unlink();break
            time.sleep(2)
