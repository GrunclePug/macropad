#!/usr/bin/env python3
"""Restart soak on the self-test build: N cycles of TESTCORRUPT (a restart
into recovery) then COMMIT from recovery (a restart into normal mode), each
checked by HELLO after the pad comes back.

usage: soak.py SERIAL N
Prints one PASS line per cycle that came back both times, FAIL otherwise, and
stops at the first pad that doesn't come back within 20 s.
"""
import glob, os, sys, time
import serial

sn, n = sys.argv[1], int(sys.argv[2])
def find(timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        for p in glob.glob('/dev/serial/by-id/*%s*' % sn):
            return os.path.realpath(p)
        time.sleep(0.1)
    return None

def opn():
    p = find()
    if not p: raise RuntimeError('pad not found')
    for _ in range(20):
        try:
            s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.005
            s.dtr = True; s.rts = True; s.open(); return s
        except Exception:
            time.sleep(0.2)
    raise RuntimeError('open failed')

def close(s):
    try: s.reset_output_buffer()
    except Exception: pass
    try: s.close()
    except Exception: pass

def knock(s):
    s.reset_output_buffer(); s.rts = True; s.dtr = False; time.sleep(0.5); s.dtr = True; time.sleep(0.05)
    s.reset_input_buffer()

buf = b''
logs = []
def line(s, timeout):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        if b'\n' in buf:
            l, buf = buf.split(b'\n', 1)
            return l.decode(errors='replace').rstrip('\r')
        try: buf += s.read(512)
        except Exception: return None
    return None

def req(s, cmd, timeout=2.0):
    """Send a command; return (lines before the final, final line)."""
    s.write((cmd + '\n').encode())
    got = []
    end = time.time() + timeout
    while time.time() < end:
        l = line(s, end - time.time())
        if l is None: break
        if l.startswith('# '):
            logs.append(l[2:]); continue
        if l == 'OK' or l.startswith('OK\t') or l.startswith('ERR\t'):
            return got, l
        got.append(l)
    return got, None

def hello(s):
    _, l = req(s, 'HELLO')
    if not l or not l.startswith('OK\tmacropad'): return None
    d = {}
    for f in l.split('\t')[2:]:
        if ' ' in f:
            k, v = f.split(' ', 1); d[k] = v
    return d

def connect(do_knock=True, tries=12):
    """Open; HELLO, knocking when unanswered."""
    global buf
    for i in range(tries):
        buf = b''
        try:
            s = opn()
        except Exception:
            time.sleep(0.5); continue
        h = hello(s)
        if h: return s, h
        if do_knock:
            knock(s); buf = b''
            h = hello(s)
            if h: return s, h
        close(s); time.sleep(0.5)
    return None, None

t0 = time.time()
for k in range(1, n + 1):
    s, h = connect()
    if not h: print('FAIL cycle %d: no HELLO before TESTCORRUPT' % k, flush=True); sys.exit(1)
    s.write(b'TESTCORRUPT\n'); close(s); time.sleep(1.5)
    s, h = connect(do_knock=False, tries=40)
    if not h or h['mode'] != 'recovery':
        print('FAIL cycle %d: back after TESTCORRUPT: %s' % (k, h), flush=True); sys.exit(1)
    _, cm = req(s, 'COMMIT', 4.0); close(s); time.sleep(1.0)
    s, h2 = connect(tries=40)
    ok = bool(cm and cm.startswith('OK')) and bool(h2) and h2['cfg'] == 'ok' and h2['boot'] != h['boot']
    print('%s cycle %d (%.0f s): recovery boot %s, then %s' % ('PASS' if ok else 'FAIL', k, time.time() - t0, h['boot'], h2), flush=True)
    if s: close(s)
    if not ok: sys.exit(1)
