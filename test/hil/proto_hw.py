#!/usr/bin/env python3
"""Protocol checks on a pad running the self-test build.

usage: proto_hw.py SERIAL LOG
Steps (each prints PASS/FAIL):
  1. knock into config mode; HELLO
  2. HELLO flood (300 back to back): every one answered
  3. GET x22, PUT x22 (unchanged), CHECK, COMMIT; reboot check (nonce, cfg ok, crc)
  4. TESTCORRUPT: recovery with cfg invalid 4
  5. DTR-only break mid-PUT: staging back to the default, still recovery
  6. TESTDETACH mid-PUT: the same after the USB-level break
  7. COMMIT the staged default: cfg ok
  8. TESTCRASH with a valid record: cfg default crash, crc unchanged
(A rerun of the self-test and its report: rerun.py.)
"""
import glob, os, sys, time
import serial

sn, logpath = sys.argv[1], sys.argv[2]
log = open(logpath, 'w')
results = []
T0 = time.monotonic()

def say(s):
    line = '[%6.1f] %s' % (time.monotonic() - T0, s)
    print(line, flush=True); log.write(line + '\n'); log.flush()

def check(name, ok, extra=''):
    results.append((name, ok))
    say('%s %s %s' % ('PASS' if ok else 'FAIL', name, extra))

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

def get_record(s):
    rec = b''
    while len(rec) < 1355:
        n = min(64, 1355 - len(rec))
        got, l = req(s, 'GET\t%d\t%d' % (len(rec), n))
        if l != 'OK': return None
        rec += bytes.fromhex(got[-1].split('\t')[1])
    return rec

def put_record(s, rec):
    for off in range(0, len(rec), 64):
        _, l = req(s, 'PUT\t%d\t%s' % (off, rec[off:off + 64].hex()))
        if l != 'OK': return False
    return True

def crc_of(rec): return '%08x' % int.from_bytes(rec[12:16], 'little')

# 1
s, h = connect()
check('knock into config mode', bool(h) and h.get('mode') == 'config', str(h))
# 2: a request/response flood, as fast as the link allows (the protocol is
# one request at a time; deep pipelining is a separate residual).
t = time.time(); oks = 0
for i in range(300):
    if hello(s): oks += 1
check('HELLO flood: 300 of 300 answered', oks == 300, '%d in %.1f s' % (oks, time.time() - t))
# 3
rec = get_record(s)
check('GET x22 reads the record', rec is not None and len(rec) == 1355)
ok = put_record(s, rec)
_, c = req(s, 'CHECK')
_, cm = req(s, 'COMMIT')
check('PUT x22, CHECK, COMMIT', ok and c == 'OK' and cm and cm.startswith('OK\t'), str(cm))
boot0 = h['boot']
close(s); time.sleep(1.0)
s, h2 = connect()
check('restart verified', bool(h2) and h2['boot'] != boot0 and h2['cfg'] == 'ok' and h2['crc'] == crc_of(rec), str(h2))
# 4
s.write(b'TESTCORRUPT\n'); close(s); time.sleep(1.5)
s, h = connect(do_knock=False)
check('TESTCORRUPT boots into recovery', bool(h) and h['mode'] == 'recovery' and h['cfg'].startswith('invalid'), str(h))
default = get_record(s)
# 5
changed = bytearray(default); changed[62] = 9  # release debounce
_, l = req(s, 'PUT\t0\t%s' % bytes(changed[0:64]).hex())
s.dtr = False; time.sleep(0.2); s.dtr = True; time.sleep(0.1)
buf = b''
got = get_record(s)
h = hello(s)
check('DTR-only break mid-PUT: staging reset, still recovery', l == 'OK' and got == default and h and h['mode'] == 'recovery', str(h))
# 6
_, l = req(s, 'PUT\t0\t%s' % bytes(changed[0:64]).hex())
s.write(b'TESTDETACH\n'); time.sleep(0.2); close(s); time.sleep(3)
s, h = connect(do_knock=False)
got = get_record(s) if s else None
check('USB-level break mid-PUT: staging reset, still recovery', l == 'OK' and got == default and h and h['mode'] == 'recovery', str(h))
# 7
_, cm = req(s, 'COMMIT')
close(s); time.sleep(1.0)
s, h = connect()
check('COMMIT from recovery: cfg ok', bool(h) and h['cfg'] == 'ok' and h['crc'] == crc_of(default), str(h))
# 8
crc = h['crc']
s.write(b'TESTCRASH\n'); close(s); time.sleep(1.5)
s, h = connect()
check('TESTCRASH: default crash, record untouched', bool(h) and h['cfg'] == 'default crash' and h['crc'] == crc, str(h))
req(s, 'EXIT'); close(s)
say('%d checks, %d failed' % (len(results), sum(1 for _, ok in results if not ok)))
