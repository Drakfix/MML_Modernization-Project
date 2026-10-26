"""Client for ghidra_server.py: python gq.py CMD [ARGS...]"""
import json
import socket
import sys

if len(sys.argv) < 2:
    sys.exit(__doc__)
s = socket.create_connection(("127.0.0.1", 47321))
s.sendall((json.dumps({"cmd": sys.argv[1], "args": sys.argv[2:]}) + "\n").encode())
data = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
print(data.decode("utf-8", "replace"))
