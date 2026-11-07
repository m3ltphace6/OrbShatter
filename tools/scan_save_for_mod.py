import sys,re
sys.path.insert(0,"tools")
from tank_reader import TankReader
with TankReader(sys.argv[1]) as t:
    for e in t.entries():
        d=t.payload(e, 64*1024*1024)
        for m in (b"OCT_",b".dsdll",b"ikkyo"):
            for x in re.finditer(re.escape(m),d):
                s=d[max(0,x.start()-60):x.end()+60]
                print(m, len(d), repr(s))
