#!/usr/bin/env python3
"""
set_license_key.py - turns your MASTER SECRET into the 16-byte licence key and (optionally) writes it into
config.h as LICENSE_AES_KEY_HEX.

  python set_license_key.py                       finds config.h automatically (or asks for its path)
  python set_license_key.py path/to/config.h      use this config.h
  python set_license_key.py --show                only print the key and key check, change nothing

Works on Windows, macOS and Linux (Python 3.6+). The master secret itself is NOT written anywhere.
Use the SAME secret in the activation key generator page and compare the printed "key check" with the generator's.
"""
import sys, os, re, hashlib, getpass, unicodedata, traceback

def ask_secret(prompt):
    try:
        return getpass.getpass(prompt)
    except Exception:                       # some terminals/IDEs have no hidden input: fall back to visible input
        print("(this terminal cannot hide typing; the secret will be visible)")
        return input(prompt)

def find_config(args):
    paths = [a for a in args if not a.startswith("--")]
    here = os.path.dirname(os.path.abspath(__file__))
    cands = paths + [os.path.join(here, "..", "CardioRespMonitor", "config.h"), os.path.join(here, "config.h"),
                     os.path.join(os.getcwd(), "config.h"), os.path.join(os.getcwd(), "CardioRespMonitor", "config.h")]
    for c in cands:
        if os.path.isfile(c):
            return os.path.abspath(c)
    return None

def main():
    args = sys.argv[1:]
    show_only = "--show" in args
    secret = ask_secret("Master secret (same as in the key generator): ").strip("\r\n")
    if len(secret) < 8:
        print("Use a longer secret (at least 8 characters, ideally 20 or more). Nothing changed.")
        return 1
    if not show_only and ask_secret("Type it again to confirm: ").strip("\r\n") != secret:
        print("The two entries differ. Nothing changed.")
        return 1
    key = hashlib.pbkdf2_hmac("sha256", unicodedata.normalize("NFC", secret).encode("utf-8"), b"esp32-vibmeter/license/v1", 100000, 16)
    kcv = hashlib.sha256(b"KCV" + key).digest()[:4].hex().upper()
    print("\nAES key (hex):", key.hex())
    print("Key check    :", kcv, "  <- must equal the 'Key check' shown in the generator")
    if show_only:
        return 0
    cfg = find_config(args)
    if not cfg:
        p = input("\nCould not find config.h. Type or drag its full path here (or press Enter to skip): ").strip().strip('"')
        cfg = p if p and os.path.isfile(p) else None
    if not cfg:
        print('\nconfig.h not updated. Paste the key by hand:\n  #define LICENSE_AES_KEY_HEX   "%s"' % key.hex())
        return 0
    with open(cfg, "rb") as f:
        raw = f.read()
    txt = raw.decode("utf-8", errors="surrogateescape")
    new, n = re.subn(r'(#define[ \t]+LICENSE_AES_KEY_HEX[ \t]+)"[0-9A-Fa-f]*"', lambda m: m.group(1) + '"%s"' % key.hex(), txt)
    if n != 1:
        print('\nLICENSE_AES_KEY_HEX line not found in %s. Paste the key by hand:\n  #define LICENSE_AES_KEY_HEX   "%s"' % (cfg, key.hex()))
        return 0
    with open(cfg + ".bak", "wb") as f:
        f.write(raw)
    with open(cfg, "wb") as f:                # bytes in, bytes out: line endings and encoding stay as they were
        f.write(new.encode("utf-8", errors="surrogateescape"))
    print("\nWritten to", cfg, "(backup: config.h.bak). Now rebuild and flash.")
    return 0

if __name__ == "__main__":
    code = 1
    try:
        code = main()
    except KeyboardInterrupt:
        print("\nCancelled.")
    except Exception:
        print("\nSomething went wrong:\n")
        traceback.print_exc()
    if sys.platform.startswith("win") and sys.stdin and sys.stdin.isatty():
        input("\nPress Enter to close...")
    sys.exit(code)
