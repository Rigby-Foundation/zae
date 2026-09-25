# -*- coding: utf-8 -*-
# python2 -m sictest: does this Python work on sic? Exercises what Ren'Py
# and the standard library lean on: built-in modules, files, fork/exec,
# threads, sockets, select, time, zlib, unicode, pickle, json, re.
from __future__ import print_function
import sys, os, time, threading, socket, select, zlib, struct, math, json, re, cPickle, StringIO, io
import datetime, random, collections, itertools, functools, hashlib, tempfile, errno, subprocess, unicodedata

fails = []
def check(cond, what):
    if not cond: fails.append(what); print("  FAIL:", what)

print("sictest: python", sys.version.split()[0], "on", sys.platform, "prefix", sys.prefix)

check(zlib.decompress(zlib.compress(b"hello" * 100)) == b"hello" * 100, "zlib")
check(struct.unpack("<I", struct.pack("<I", 0xdeadbeef))[0] == 0xdeadbeef, "struct")
check(abs(math.sin(math.pi / 2) - 1) < 1e-9, "math")
check(json.loads(json.dumps({"a": [1, 2.5, u"é"]})) == {"a": [1, 2.5, u"é"]}, "json")
check(re.sub(r"(\w+) (\w+)", r"\2 \1", "hello world") == "world hello", "re")
check(cPickle.loads(cPickle.dumps([1, "two", 3.0], 2)) == [1, "two", 3.0], "cPickle")
check(hashlib.md5(b"abc").hexdigest() == "900150983cd24fb0d6963f7d28e17f72", "hashlib md5")
check(hashlib.sha256(b"abc").hexdigest().startswith("ba7816bf"), "hashlib sha256")
check(unicodedata.name(u"é") == "LATIN SMALL LETTER E WITH ACUTE", "unicodedata")
check(u"naïve".encode("utf-8").decode("utf-8") == u"naïve", "codecs")
check(datetime.date(2026, 9, 18).isoformat() == "2026-09-18", "datetime")
check(collections.Counter("abracadabra")["a"] == 5, "collections")
check(list(itertools.islice(itertools.count(), 3)) == [0, 1, 2], "itertools")

# files
d = tempfile.mkdtemp(dir="/tmp") if os.path.isdir("/tmp") else "/"
p = os.path.join(d, "sictest.txt")
with open(p, "w") as f: f.write("line1\nline2\n")
with open(p) as f: check(f.readlines() == ["line1\n", "line2\n"], "file write/read")
check(os.stat(p).st_size == 12, "os.stat")
check("sictest.txt" in os.listdir(d), "os.listdir")
os.remove(p)
check(not os.path.exists(p), "os.remove")

# time
t0 = time.time(); time.sleep(0.05); dt = time.time() - t0
check(0.04 < dt < 0.5, "time.sleep %.3f" % dt)
check(time.strftime("%Y", time.gmtime(0)) == "1970", "strftime")

# fork/exec/pipes
pid = os.fork()
if pid == 0:
    os._exit(7)
_, st = os.waitpid(pid, 0)
check(os.WEXITSTATUS(st) == 7, "fork/waitpid")
out = subprocess.check_output(["/bin/echo", "sub", "process"])
check(out.strip() == b"sub process", "subprocess.check_output")
r, w = os.pipe()
os.write(w, b"pipe"); check(os.read(r, 4) == b"pipe", "os.pipe"); os.close(r); os.close(w)

# threads
res = []
def worker(n):
    s = 0
    for i in range(n): s += i
    res.append(s)
ts = [threading.Thread(target=worker, args=(10000,)) for _ in range(4)]
for t in ts: t.start()
for t in ts: t.join()
check(res == [49995000] * 4, "threads")
lock = threading.Lock(); ev = threading.Event()
threading.Timer(0.05, ev.set).start()
check(ev.wait(2), "threading.Event/Timer")

# sockets: a TCP echo over loopback, and select on it
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0)); srv.listen(1)
port = srv.getsockname()[1]
def echo():
    c, _ = srv.accept(); c.sendall(c.recv(100)); c.close()
threading.Thread(target=echo).start()
cl = socket.create_connection(("127.0.0.1", port), timeout=5)
cl.sendall(b"ping")
rl, _, _ = select.select([cl], [], [], 5)
check(rl == [cl] and cl.recv(100) == b"ping", "tcp echo + select")
cl.close(); srv.close()
if hasattr(socket, "AF_UNIX"):
    us = socket.socket(socket.AF_UNIX); us.bind("/tmp/sictest.sock"); us.listen(1)
    def uecho():
        c, _ = us.accept(); c.sendall(c.recv(100)); c.close()
    threading.Thread(target=uecho).start()
    uc = socket.socket(socket.AF_UNIX); uc.connect("/tmp/sictest.sock"); uc.sendall(b"unix")
    check(uc.recv(100) == b"unix", "unix socket echo")
    uc.close(); us.close()

# the compiler and imports
exec(compile("x = [i * i for i in range(5)]", "<t>", "exec"))
check(x == [0, 1, 4, 9, 16], "compile/exec")
import xml.etree.ElementTree as ET
check(ET.fromstring("<a><b>t</b></a>").find("b").text == "t", "xml.etree (expat)")
import csv
check(list(csv.reader(StringIO.StringIO("a,b\n1,2\n"))) == [["a", "b"], ["1", "2"]], "csv")
check(io.BytesIO(b"io").read() == b"io", "io")

print("sictest:", "ok" if not fails else "%d failure(s)" % len(fails))
sys.exit(1 if fails else 0)
