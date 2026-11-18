import glob, os, sys, time, serial
p = os.path.realpath(glob.glob('/dev/serial/by-id/*%s*' % sys.argv[1])[0])
s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.05; s.dtr = True; s.rts = True; s.open()
s.write(b'HELLO\n'); buf = b''; end = time.time() + 0.3
while time.time() < end: buf += s.read(512)
if b'OK\tmacropad' not in buf:
    s.reset_output_buffer(); s.dtr = False; time.sleep(0.5); s.dtr = True; time.sleep(0.3)
    s.reset_input_buffer(); s.write(b'HELLO\n'); buf = b''; end = time.time() + 1
    while time.time() < end: buf += s.read(512)
print('quiet:', [l for l in buf.decode(errors='replace').split('\n') if l.startswith('OK\tmacropad')][:1])
s.close()
