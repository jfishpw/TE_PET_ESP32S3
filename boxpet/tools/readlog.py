# readlog.py - 读取 COM5 串口日志 N 秒后退出（临时调试工具）
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 1
s.dtr = False   # 避免开 port 时拉复位/引导脚
s.rts = False
s.open()
# 复位脉冲（USB-Serial/JTAG：RTS 控 EN）
s.rts = True
time.sleep(0.1)
s.rts = False
end = time.time() + secs
buf = b''
while time.time() < end:
    buf += s.read(4096)
s.close()
sys.stdout.write(buf.decode('utf-8', 'replace'))
