"""Put the pad in config mode (idle) before an upload, and keep it there.

usage: quiet.py SERIAL   exits 1 if the pad didn't answer HELLO in config mode
The port is closed without dropping DTR (HUPCL off), so closing it doesn't end
the config session before the upload's own reset.
"""
import glob, os, sys, termios, time, serial
p = os.path.realpath(glob.glob('/dev/serial/by-id/*%s*' % sys.argv[1])[0])
s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.05; s.write_timeout = 1; s.dtr = True; s.rts = True; s.open()
buf = b''
try:
    s.write(b'HELLO\n'); end = time.time() + 0.3
    while time.time() < end: buf += s.read(512)
except serial.SerialTimeoutException: pass   # normal mode: the pad doesn't read
if b'OK\tmacropad' not in buf:
    s.reset_output_buffer(); s.dtr = False; time.sleep(0.5); s.dtr = True; time.sleep(0.3)
    s.reset_input_buffer(); s.write(b'HELLO\n'); buf = b''; end = time.time() + 1
    while time.time() < end: buf += s.read(512)
hello = [l for l in buf.decode(errors='replace').split('\n') if l.startswith('OK\tmacropad')][:1]
print('quiet:', hello)
attrs = termios.tcgetattr(s.fd)
attrs[2] &= ~termios.HUPCL
termios.tcsetattr(s.fd, termios.TCSANOW, attrs)
s.close()
sys.exit(0 if hello and '\tmode ' in hello[0] else 1)
