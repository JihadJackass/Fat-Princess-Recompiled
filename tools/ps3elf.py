"""PS3 PPU ELF helpers: segment mapping, PRX import/export parsing, NID database."""
import struct, hashlib, re, os, glob
SUFFIX=bytes.fromhex("6759659904250490566427499489741A")
def nid(name): return struct.unpack('<I',hashlib.sha1(name.encode()+SUFFIX).digest()[:4])[0]

class Elf:
    def __init__(s,path):
        s.d=open(path,'rb').read(); d=s.d
        s.entry=struct.unpack('>Q',d[0x18:0x20])[0]
        phoff=struct.unpack('>Q',d[0x20:0x28])[0]; phn=struct.unpack('>H',d[0x38:0x3a])[0]
        s.ph=[]
        for i in range(phn):
            t,fl,off,va,pa,fs,ms,al=struct.unpack('>IIQQQQQQ',d[phoff+i*56:phoff+i*56+56])
            s.ph.append(dict(type=t,flags=fl,off=off,va=va,fs=fs,ms=ms))
    def rd(s,va,n):
        for p in s.ph:
            if p['type']==1 and p['va']<=va<p['va']+p['fs']:
                o=p['off']+va-p['va']; return s.d[o:o+n]
        return b'\0'*n
    def u32(s,va): return struct.unpack('>I',s.rd(va,4))[0]
    def cstr(s,va):
        b=s.rd(va,256); return b[:b.index(b'\0')].decode('latin1')
    def prx(s):
        p=[x for x in s.ph if x['type']==0x60000002][0]
        b=s.d[p['off']:p['off']+0x40]
        f=struct.unpack('>IIIIIIII',b[:32])
        return dict(libent=(f[4],f[5]),libstub=(f[6],f[7]))
    def imports(s):
        a,e=s.prx()['libstub']; out=[]
        while a<e:
            b=s.rd(a,0x2c); sz=b[0]
            (nf,nv,nt)=struct.unpack('>HHH',b[6:12])
            name_a,fnid_a,fstub_a,vnid_a,vstub_a=struct.unpack('>IIIII',b[0x10:0x24])
            fn=[(s.u32(fnid_a+4*i), s.u32(fstub_a+4*i)) for i in range(nf)]
            vr=[(s.u32(vnid_a+4*i), s.u32(vstub_a+4*i)) for i in range(nv)]
            out.append(dict(lib=s.cstr(name_a),funcs=fn,vars=vr)); a+=sz
        return out

def nid_db(rpcs3_root):
    db={}
    for f in glob.glob(rpcs3_root+'/rpcs3/Emu/Cell/**/*.cpp',recursive=True):
        t=open(f,errors='ignore').read()
        for m in re.finditer(r'REG_(?:FUNC|VAR|HIDDEN_FUNC)\(\s*\w+\s*,\s*(\w+)',t): db[nid(m.group(1))]=m.group(1)
        for m in re.finditer(r'REG_FNID\(\s*\w+\s*,\s*"([^"]+)"',t): db[nid(m.group(1))]=m.group(1)
        for m in re.finditer(r'REG_FNID\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)',t): db[int(m.group(1),16)]=m.group(2)
    return db
