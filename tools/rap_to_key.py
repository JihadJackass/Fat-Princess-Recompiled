"""Print the DLC licence key for a .rap licence file you own.

Usage:  python tools\\rap_to_key.py "C:\\path\\to\\your.rap"
Needs:  pip install psntools pycryptodome
Paste the 32-character result into the launcher: Paths tab, Fat Roles DLC key.
"""
import binascii
import sys

try:
    from Crypto.Cipher import AES
    from psntools import raprif
except ImportError:
    sys.exit("Missing packages. Run:  pip install psntools pycryptodome")

if len(sys.argv) != 2:
    sys.exit('Usage: python tools\\rap_to_key.py "C:\\path\\to\\your.rap"')

with open(sys.argv[1], "rb") as f:
    rap = f.read()
if len(rap) < 16:
    sys.exit("That file is too small to be a .rap licence (expected 16 bytes).")

step1 = AES.new(bytes(raprif.rap_aes_key), AES.MODE_ECB).decrypt(rap[:16])
key = bytes(raprif.rap2rif_rounds(step1))
print(binascii.hexlify(key).decode().upper())
