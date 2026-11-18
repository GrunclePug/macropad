import glob, os, sys, time, serial
p = os.path.realpath(glob.glob('/dev/serial/by-id/*%s*' % sys.argv[1])[0])
s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.05; s.write_timeout = 1; s.dtr = True; s.rts = True; s.open()
s.reset_output_buffer(); s.dtr = False; time.sleep(0.5); s.dtr = True; time.sleep(1); s.reset_input_buffer()
for c in (b'HELLO\n', b'COMMIT\n'):
    s.write(c); buf = b''; end = time.time() + 2
    while time.time() < end and b'OK' not in buf and b'ERR' not in buf: buf += s.read(512)
    print(buf.decode(errors='replace').strip()[-120:])
s.close()
