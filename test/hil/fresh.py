import glob, os, sys, time, zlib, serial
def opn():
    for _ in range(150):
        g = glob.glob('/dev/serial/by-id/*%s*' % sys.argv[1])
        if g:
            try:
                s = serial.Serial(); s.port = os.path.realpath(g[0]); s.baudrate = 115200; s.timeout = 0.05; s.dtr = True; s.rts = True; s.open(); return s
            except Exception: pass
        time.sleep(0.1)
def req(s, c):
    s.write(c); buf = b''; end = time.time() + 2
    while time.time() < end:
        buf += s.read(512)
        lines = buf.decode(errors='replace').split('\n')
        if [l for l in lines if l == 'OK' or l.startswith('OK\t') or l.startswith('ERR')]:
            return [l for l in lines if not l.startswith('# ')]
    return []
def knock():
    s = opn(); s.reset_output_buffer()
    if not [l for l in req(s, b'HELLO\n') if l.startswith('OK\tmac')]:
        s.dtr = False; time.sleep(0.5); s.dtr = True; time.sleep(0.4); s.reset_input_buffer()
    return s
def hello(s):
    h = [l for l in req(s, b'HELLO\n') if l.startswith('OK\tmacropad')][0]
    return dict(f.split(' ', 1) for f in h.split('\t')[2:] if ' ' in f)
def record(s):
    rec = b''
    while len(rec) < 1355:
        n = min(64, 1355 - len(rec))
        rec += bytes.fromhex([l for l in req(s, ('GET\t%d\t%d\n' % (len(rec), n)).encode()) if l.startswith('DATA')][0].split('\t')[1])
    return rec
mode = sys.argv[2]
s = knock()
if mode == 'erase':
    req(s, b'TESTERASE\tnorun\n'); s.close(); time.sleep(3)
    time.sleep(5)   # past the script's 3 s start: norun must hold it
    s = knock(); h = hello(s); print('after TESTERASE norun (8 s):', h['cfg'], h['crc'])
    running = record(s); before = '%08x' % int.from_bytes(running[12:16], 'little')
    req(s, b'TESTTAP\n'); req(s, b'EXIT\n'); s.close(); time.sleep(1.5)   # a pad edit: the live flag
    s = knock(); h = hello(s); rec = record(s)
    stored = '%08x' % int.from_bytes(rec[12:16], 'little')
    print('after a pad edit and a knock: cfg', h['cfg'], 'crc', h['crc'], 'GET record crc', stored, '(before the edit', before + ')')
    ok = h['cfg'] == 'ok' and h['crc'] == stored and stored != before
    print('PASS' if ok else 'FAIL', 'fresh pad: entry creates the record with the edit; HELLO matches GET')
    req(s, b'TESTERASE\tnorun\n'); s.close(); time.sleep(3)   # fresh again and idle, for the push
    s = knock(); h = hello(s); req(s, b'EXIT\n'); s.close(); time.sleep(1)
    print('PASS' if h['cfg'] == 'none' and h['crc'] == 'none' else 'FAIL', 'erased again for the push:', h['cfg'], h['crc'])
elif mode == 'corrupt':
    req(s, b'TESTCORRUPT\n'); s.close(); time.sleep(3)
    s = knock(); h = hello(s); print('after TESTCORRUPT:', h['mode'], h['cfg']); s.close()
elif mode == 'reset':
    req(s, b'TESTERASE\n'); s.close(); time.sleep(3)   # boots the compiled fixture
elif mode == 'status':
    h = hello(s); print('status:', h['mode'], h['cfg'], h['crc']); req(s, b'EXIT\n'); s.close()
