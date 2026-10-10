#!/usr/bin/env python3
"""Reject unrecognized radio entry points before preserving an R1 backend."""
import hashlib
import pathlib
import sys


def verify(root: pathlib.Path) -> None:
    repo = pathlib.Path(__file__).resolve().parents[2]
    helper = root / 'usr/bin/compas-radio'
    expected = repo / 'firmware/kernel/wifi-experimental/rootfs/usr/bin/compas-radio'
    if helper.is_symlink() or helper.read_bytes() != expected.read_bytes():
        raise ValueError('Unrecognized Compas radio backend')
    for name, command in [('wifi_on.sh', 'wifi-up'), ('wifi_off.sh', 'wifi-down')]:
        path = root / 'usr/bin' / name
        if path.is_symlink() or path.read_text() != '#!/bin/sh\nexec /usr/bin/compas-radio ' + command + ' "$@"\n':
            raise ValueError('Unrecognized Compas radio shim: ' + name)
    startup = root / 'etc/init.d/S43wifi_bcm_init_config'
    # Exact guarded start/stop script emitted by stage_experimental_radio.py.
    if startup.is_symlink() or hashlib.sha256(startup.read_bytes()).hexdigest() != '1cfed042a65a0a19bd586ba8daca82bf0a304040236fed581d838b9af7390aa4':
        raise ValueError('Unrecognized Compas radio S43 startup')
    for name in ['wifi_on.brcmfmac.vendor.sh', 'wifi_off.brcmfmac.vendor.sh']:
        if not (root / 'usr/libexec/compas' / name).is_file():
            raise ValueError('Missing guarded radio vendor script: ' + name)


if __name__ == '__main__':
    try:
        verify(pathlib.Path(sys.argv[1]))
    except (OSError, ValueError) as exc:
        sys.exit(str(exc))
