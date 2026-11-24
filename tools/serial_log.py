# Read-only serial capture: opens the CDC port, raises DTR, logs lines with host time. Never writes to the device.
import os, sys, termios, fcntl, time, select
port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; a[3] = 0
a[4] = a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)
TIOCMBIS = 0x8004746C; TIOCM_DTR = 0x002; TIOCM_RTS = 0x004
fcntl.ioctl(fd, TIOCMBIS, TIOCM_DTR.to_bytes(4, 'little'))
buf = b''; t0 = time.time()
with open(out, 'a', buffering=1) as f:
    f.write(f'# capture start {time.strftime("%H:%M:%S")}\n')
    while time.time() - t0 < secs:
        r, _, _ = select.select([fd], [], [], 0.5)
        if not r: continue
        try: d = os.read(fd, 4096)
        except BlockingIOError: continue
        buf += d
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            f.write(f'{time.time()-t0:8.2f} {line.decode("ascii","replace").rstrip()}\n')
os.close(fd)
