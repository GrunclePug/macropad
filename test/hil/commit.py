import glob, os, re, sys, time, serial
p = os.path.realpath(glob.glob('/dev/serial/by-id/*%s*' % sys.argv[1])[0])
s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.05; s.write_timeout = 1; s.dtr = True; s.rts = True; s.open()
s.reset_output_buffer(); s.dtr = False; time.sleep(0.5); s.dtr = True; time.sleep(1); s.reset_input_buffer()
last = ''
for c in (b'HELLO\n', b'COMMIT\n'):
    s.write(c); buf = b''; end = time.time() + 2
    while time.time() < end and b'OK' not in buf and b'ERR' not in buf: buf += s.read(512)
    last = buf.decode(errors='replace').strip()
    print(last[-120:])
s.close()
# COMMIT answers OK and a crc, then restarts.
sys.exit(0 if any(re.fullmatch(r'OK\t[0-9a-f]{8}', l.strip()) for l in last.split('\n')) else 1)
