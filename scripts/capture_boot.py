from pathlib import Path
import argparse,time
import serial
from esptool.reset import HardReset

p=argparse.ArgumentParser();p.add_argument('--seconds',type=int,default=15)
a=p.parse_args()
s=serial.Serial(port=None,baudrate=921600,timeout=.1)
s.dtr=False;s.rts=False;s.port='COM9';s.open()
HardReset(s,uses_usb=True)()
end=time.monotonic()+a.seconds
target=Path(__file__).resolve().parents[1]/'.local/boot.log'
with target.open('wb') as f:
    while time.monotonic()<end:
        b=s.read(s.in_waiting or 1)
        if b:f.write(b);f.flush()
s.close()
print(target)
