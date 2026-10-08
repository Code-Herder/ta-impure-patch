"""sintab.py -- read the engine's sine table (0x509F00, 512 int16) from the pristine exe."""
import struct, numpy as np
EXE = __import__("os").environ.get("TA_PRISTINE", "pristine/TotalA.exe.pristine")
def va_bytes(va, n):
    d = open(EXE, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    sh = pe + 24 + opt
    for i in range(nsec):
        name, vsize, vaddr, rsize, rptr = struct.unpack_from("<8sIIII", d, sh + 40 * i)
        if vaddr <= va - base < vaddr + max(vsize, rsize):
            off = va - base - vaddr
            return d[rptr + off: rptr + off + n], name
table, sec = va_bytes(0x509F00, 1024)
T = np.frombuffer(table, "<i2")
if __name__ == "__main__":
    print(sec, T[:8], T[120:136], T.max(), T.min(), T[128], T[256], T[384])
