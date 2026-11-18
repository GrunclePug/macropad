import glob, os, sys, time, serial
def opn():
    for _ in range(150):
        g = glob.glob('/dev/serial/by-id/*%s*' % sys.argv[1])
        if g:
            try:
                s = serial.Serial(); s.port = os.path.realpath(g[0]); s.baudrate = 115200; s.timeout = 0.05; s.dtr = True; s.rts = True; s.open(); return s
            except Exception: pass
        time.sleep(0.1)
def knock_read(secs):
    s = opn(); s.reset_output_buffer(); s.dtr = False; time.sleep(0.5); s.dtr = True
    buf = b''; end = time.time() + secs
    while time.time() < end: buf += s.read(4096)
    return s, buf.decode(errors='replace')
step, state = sys.argv[2], sys.argv[3]   # state: a file that carries the first report's summary
if step == 'first':      # the boot run's report: read, not acknowledged
    s, out = knock_read(4)
    summaries = [l for l in out.split('\n') if 'checks,' in l]
    print('first report:', summaries)
    print('PASS' if summaries else 'FAIL', "the boot run's report is handed out")
    open(state, 'w').write(summaries[-1] if summaries else '')
    s.write(b'EXIT\n'); time.sleep(0.5); s.close()
elif step == 'rerun':    # TESTRUN without TESTACK, then cut the new run short
    first = open(state).read()
    s, out = knock_read(1)
    s.write(b'TESTRUN\n'); time.sleep(0.5); s.close()
    time.sleep(12)       # the new run is under way (it starts 3 s after leaving)
    s, out = knock_read(5)
    summaries = [l for l in out.split('\n') if 'checks,' in l]
    print('after TESTRUN and a knock mid-run:', summaries, '| stopped:', 'stopped: mode changed' in out)
    ok = first and len(summaries) == 1 and 'stopped: mode changed' in out and summaries[0] != first
    print('PASS' if ok else 'FAIL', 'a rerun never hands out the previous report')
    s.write(b'EXIT\n'); time.sleep(0.5); s.close()
