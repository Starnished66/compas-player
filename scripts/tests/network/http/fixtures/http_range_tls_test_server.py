#!/usr/bin/env python3
import http.server
import ssl
import sys

port_file, cert, key = sys.argv[1:]
entity = (b"0123456789" * 30000)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        value = self.headers.get("Range", "")
        start = int(value.removeprefix("bytes=").removesuffix("-")) if value else 0
        if value:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(entity)-1}/{len(entity)}")
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(len(entity) - start))
        self.send_header("ETag", '"v1"')
        self.end_headers()
        self.wfile.write(entity[start:])

    def log_message(self, *_args):
        pass


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.minimum_version = ssl.TLSVersion.TLSv1_2
context.maximum_version = ssl.TLSVersion.TLSv1_2
context.load_cert_chain(certfile=cert, keyfile=key)
server.socket = context.wrap_socket(server.socket, server_side=True)
with open(port_file, "w", encoding="ascii") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
