#!/usr/bin/env python3
"""Generate one ESP32 P-256 signing key and its public-key registration file."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

sketch = Path(__file__).resolve().parents[1] / '1_Esp_pelumi_project'
header = sketch / 'device-signing-secret.h'
public = sketch / 'public-key.pem'
if header.exists() or public.exists():
    sys.exit('Existing key files found. Refusing to overwrite this meter identity.')
with tempfile.TemporaryDirectory(prefix='meridian-meter-key-') as directory:
    private = Path(directory) / 'private.key'
    subprocess.run(['openssl', 'genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', str(private)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout', '-out', str(public)], check=True, stdout=subprocess.DEVNULL)
    pem = private.read_text()
    header.write_text('#pragma once\n#define DEVICE_SIGNATURE_ALGORITHM "ECDSA_P256_SHA256"\nstatic const char DEVICE_PRIVATE_KEY_PEM[] = R"MERIDIAN_KEY(' + pem + ')MERIDIAN_KEY";\n')
    os.chmod(header, 0o600)
    os.chmod(public, 0o600)
print('Created private signing header (keep on the engineer’s computer) and public-key.pem (paste into Add device).')
