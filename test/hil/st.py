#!/usr/bin/env python3
"""Run the on-device self-test under a serial host load, then knock and read
its report.

usage: st.py SERIAL MODE OUT [WAIT_S]
  idle    open the port and keep it open
  flood   write HELLO lines and junk as fast as the port takes them
  noread  write HELLO every 10 ms
The load runs for WAIT_S seconds (default 100, longer than the self-test).
Then the knock, and the report is read from config mode; TESTACK only once
its summary was read. Reopens the port when the pad drops off USB (the
self-test includes a USB detach). Exits 1 if no report was read.
"""
import glob, os, select, sys, threading, time
import serial

sn, mode, out = sys.argv[1], sys.argv[2], sys.argv[3]
WAIT_S = float(sys.argv[4]) if len(sys.argv) > 4 else 100
log = open(out, 'w')
start = time.monotonic()
def t(): return time.monotonic() - start
def note(s):
    log.write('[%.2f %s]\n' % (t(), s)); log.flush()

def find_tty():
    for p in glob.glob('/dev/serial/by-id/*%s*' % sn): return os.path.realpath(p)

def find_hidraw():
    for d in glob.glob('/sys/class/hidraw/hidraw*'):
        try: u = open(d + '/device/uevent').read().upper()
        except OSError: continue
        if '2341:00000070' in u: return '/dev/' + os.path.basename(d)

last_report = [time.monotonic()]
reports = [0]
stop = False

def hid_watch():
    fd = None
    while not stop:
        if fd is None:
            p = find_hidraw()
            if p:
                try: fd = os.open(p, os.O_RDONLY | os.O_NONBLOCK)
                except OSError: fd = None
            if fd is None: time.sleep(0.2); continue
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                os.read(fd, 64); reports[0] += 1; last_report[0] = time.monotonic()
            except OSError:
                os.close(fd); fd = None

port = None
def opener():
    global port
    p = find_tty()
    if not p: return
    try:
        s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0; s.write_timeout = 0
        s.dtr = True; s.rts = True; s.open(); port = s; note('open ' + p)
    except Exception as e:
        pass

threading.Thread(target=hid_watch, daemon=True).start()
junk = b"\x00\xffNOT A COMMAND " * 4 + b"\n"
writes = 0
last_note = -1
while True:
    now = time.monotonic()
    if t() > WAIT_S: break
    if port is None:
        opener(); time.sleep(0.05); continue
    try:
        if not os.path.exists(port.port): raise OSError('gone')
        # os.write on the non-blocking fd: pyserial's write spins forever on
        # EAGAIN when write_timeout is 0.
        if mode == 'flood':
            try: writes += os.write(port.fd, b"HELLO\n" + junk)
            except BlockingIOError: time.sleep(0.001)
        elif mode == 'noread':
            try: writes += os.write(port.fd, b"HELLO\n")
            except BlockingIOError: pass
            time.sleep(0.01)
        else:
            time.sleep(0.05)
        if mode != 'idle' and int(t()) % 10 == 0 and int(t()) != last_note:
            last_note = int(t()); note('bytes written %d' % writes)
    except Exception:
        note('port lost');
        try: port.close()
        except Exception: pass
        port = None
note('load done: %d reports, %d bytes written' % (reports[0], writes))

# Knock and read the report.
if port is None: opener()
try: port.reset_output_buffer()
except Exception: pass
port.rts = True; port.dtr = False; time.sleep(0.5); port.dtr = True
note('knocked')
buf = b''
end = time.monotonic() + 30
while time.monotonic() < end:
    try: buf += port.read(4096)
    except Exception: pass
    if b'checks,' in buf and buf.endswith(b'\n') and b'===' in buf.split(b'checks,')[-1]:
        time.sleep(0.5); buf += port.read(4096); break
    time.sleep(0.02)
got = b'checks,' in buf
if got: port.write(b"TESTACK\n"); time.sleep(0.3)   # read: it may go
port.write(b"EXIT\n"); time.sleep(0.5)
try: buf += port.read(4096)
except Exception: pass
log.write(buf.decode(errors='replace'))
stop = True
port.reset_output_buffer(); port.close()
note('end')
sys.exit(0 if got else 1)
