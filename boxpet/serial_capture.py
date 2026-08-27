# serial_capture.py - 持续捕获 COM5 串口日志（断连自动重连）
# 用于抓 Light Sleep 崩溃循环：重启后 USB CDC 重新枚举，脚本自动重连，
# 抓取 "reset reason=" 等关键日志。Ctrl+C 或超时退出。
import serial, time, sys, os

PORT = 'COM5'
BAUD = 115200
LOG = 'serial_log.txt'
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 120  # 默认抓 2 分钟

f = open(LOG, 'a', encoding='utf-8', errors='replace')
f.write('\n===== capture start %s =====\n' % time.strftime('%H:%M:%S'))
f.flush()

deadline = time.time() + DURATION
ser = None
while time.time() < deadline:
    if ser is None:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=0.2)
            f.write('[%s] connected\n' % time.strftime('%H:%M:%S'))
            f.flush()
        except Exception as e:
            time.sleep(0.3)  # CDC 断连（Light Sleep/重启中），稍后重试
            continue
    try:
        data = ser.read(4096)
        if data:
            f.write(data.decode('utf-8', errors='replace'))
            f.flush()
    except Exception:
        f.write('\n[%s] disconnected\n' % time.strftime('%H:%M:%S'))
        f.flush()
        try: ser.close()
        except Exception: pass
        ser = None
        time.sleep(0.3)

if ser:
    try: ser.close()
    except Exception: pass
f.write('===== capture end =====\n')
f.close()
print('done, log -> %s (%d bytes)' % (LOG, os.path.getsize(LOG)))
