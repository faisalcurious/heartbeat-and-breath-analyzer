#!/usr/bin/env python3
"""Drives the REAL esp32-activation-keygen.html in headless Chromium and prints a JSON line:
{"token","kcv","key_hex","mac","user"}.   usage: make_license_token.py keygen.html secret mac user id [days]"""
import sys, json, hashlib, unicodedata, datetime
from playwright.sync_api import sync_playwright
html, secret, mac, user, uid = sys.argv[1:6]
days = int(sys.argv[6]) if len(sys.argv) > 6 else 30
today = datetime.date.today()
issue = today.isoformat(); exp = (today + datetime.timedelta(days=days)).isoformat()
with sync_playwright() as p:
    b = p.chromium.launch(executable_path="/opt/pw-browsers/chromium", args=["--no-sandbox"]) if False else p.chromium.launch(args=["--no-sandbox"])
    pg = b.new_page()
    pg.goto("file://" + html)
    pg.fill("#secret", secret); pg.fill("#userName", user); pg.fill("#userId", uid); pg.fill("#mac", mac)
    pg.fill("#issueDate", issue); pg.fill("#expiryDate", exp)
    pg.wait_for_timeout(900)
    pg.click("#generateBtn")
    pg.wait_for_function("document.getElementById('tokenOut').value.length > 20", timeout=15000)
    tok = pg.input_value("#tokenOut"); kcv = pg.inner_text("#kcvValue")
    b.close()
key = hashlib.pbkdf2_hmac("sha256", unicodedata.normalize("NFC", secret).encode(), b"esp32-vibmeter/license/v1", 100000, 16).hex()
print(json.dumps(dict(token=tok, kcv=kcv, key_hex=key, mac=mac.upper(), user=user, uid=uid, issue=issue, expiry=exp)))
