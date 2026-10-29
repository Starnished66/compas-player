#!/bin/sh
# The embedded CA bundle must hold public Mozilla roots only: a locally added CA (mkcert, a
# homelab CA, a private address) would make every device trust certificates its owner can mint.
set -eu
cd "$(dirname "$0")/../.."
python3 - <<'EOF'
import re
import subprocess
import sys

src = open("src/network/security/ca_bundle.c", encoding="utf-8").read()
body = src.split("ca_bundle_pem[] = {", 1)[1].split("};", 1)[0]
pem = bytes(int(n) for n in re.findall(r"\d+", body))
declared = int(re.search(r"ca_bundle_pem_len = (\d+);", src).group(1))
if declared != len(pem) or pem[-1] != 0:
    sys.exit("ca_bundle: length or NUL terminator is wrong")
text = pem[:-1].decode("utf-8")
private = re.compile(r"mkcert|homelab|localhost|\.local\b|\.lan\b|"
                     r"\b(10|127)\.\d+\.\d+\.\d+|\b192\.168\.\d+\.\d+|\b172\.(1[6-9]|2\d|3[01])\.\d+\.\d+", re.I)
certs = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----\n", text, re.S)
if len(certs) < 100:
    sys.exit(f"ca_bundle: only {len(certs)} certificates")
for cert in certs:
    info = subprocess.run(["openssl", "x509", "-noout", "-subject", "-issuer", "-ext", "basicConstraints"],
                          input=cert, capture_output=True, text=True, check=True).stdout
    if "CA:TRUE" not in info or private.search(info):
        sys.exit("ca_bundle: not a public root CA:\n" + info)
if private.search(text):
    sys.exit("ca_bundle: private name or address in the bundle")
print(f"ca_bundle: {len(certs)} public root certificates")
EOF
