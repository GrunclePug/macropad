#!/usr/bin/env python3
"""Restart soak on the self-test build: N cycles of TESTCORRUPT (a restart
into recovery) then COMMIT from recovery (a restart into normal mode), each
checked by HELLO after the pad comes back.

usage: soak.py SERIAL N
Each restart must answer HELLO with a new boot nonce within 20 s: recovery
after TESTCORRUPT (acknowledged), then normal mode with `cfg ok` and the crc
COMMIT answered. Prints one PASS line per cycle, and stops at the first FAIL.
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

def opn(timeout=15):
    p = find(timeout)
    if not p: raise RuntimeError('pad not found')
    end = time.monotonic() + min(timeout, 4)
    while time.monotonic() < end:
        try:
            s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.005
            s.write_timeout = 1; s.dtr = True; s.rts = True; s.open(); return s
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
    try: s.write((cmd + '\n').encode())
    except serial.SerialTimeoutException: return [], None   # normal mode: the pad doesn't read
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

def back(boot0, do_knock, secs=20):
    """The pad after a restart: a HELLO with a new boot nonce, all within secs
    (one deadline). The old port may still be there for the 200 ms before the
    restart, or vanish mid-HELLO: any serial error just means try again."""
    end = time.monotonic() + secs
    while time.monotonic() < end:
        s = None
        try:
            p = find(timeout=max(0.1, end - time.monotonic()))
            if p:
                s = opn(max(0.1, end - time.monotonic()))
                h = hello(s)
                if not h and do_knock:
                    knock(s); h = hello(s)
                # Only within the deadline: a HELLO that ends after it is late.
                if h and h['boot'] != boot0 and time.monotonic() <= end: return s, h
        except Exception:
            pass
        if s: close(s)
        time.sleep(0.3)
    return None, None

def stop(msg):
    print('FAIL ' + msg, flush=True); sys.exit(1)

t0 = time.time()
for k in range(1, n + 1):
    s, h0 = connect()
    if not h0: stop('cycle %d: no HELLO before TESTCORRUPT' % k)
    _, ack = req(s, 'TESTCORRUPT'); close(s)
    if ack != 'OK': stop('cycle %d: TESTCORRUPT not acknowledged (%s)' % (k, ack))
    s, h = back(h0['boot'], do_knock=False)
    if not h or h['mode'] != 'recovery': stop('cycle %d: not back in recovery within 20 s: %s' % (k, h))
    _, cm = req(s, 'COMMIT', 4.0); close(s)
    if not cm or not cm.startswith('OK\t'): stop('cycle %d: COMMIT from recovery: %s' % (k, cm))
    want = cm.split('\t')[1]
    s, h2 = back(h['boot'], do_knock=True)
    ok = bool(h2) and h2['mode'] == 'config' and h2['cfg'] == 'ok' and h2['crc'] == want
    print('%s cycle %d (%.0f s): recovery boot %s, then %s' % ('PASS' if ok else 'FAIL', k, time.time() - t0, h['boot'], h2), flush=True)
    if s:
        req(s, 'EXIT'); close(s)
    if not ok: sys.exit(1)
