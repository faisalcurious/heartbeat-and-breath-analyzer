# Wi-Fi setup wizard, accelerometer calibration and device-card checks against the mock device
import asyncio
from playwright.async_api import async_playwright
CH = "/opt/pw-browsers/chromium-1194/chrome-linux/chrome"

async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CH, args=["--no-sandbox"])
        pg = await b.new_page(viewport={"width": 390, "height": 844}, device_scale_factor=2)
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.on("console", lambda m: errs.append(m.text) if m.type == "error" else None)
        pg.on("dialog", lambda d: asyncio.ensure_future(d.accept()))
        await pg.goto("http://localhost:8080/")
        await pg.wait_for_timeout(4000)
        in_top = await pg.evaluate("() => document.getElementById('netTop').contains(document.getElementById('netCard'))")
        print("wizard shown at top:", in_top, "| title:", await pg.text_content("#netTitle"))
        print("scan options:", await pg.evaluate("() => [...document.querySelectorAll('#wSel option')].map(o => o.textContent)"))
        await pg.screenshot(path="shot_wizard.png")
        # wrong password
        await pg.select_option("#wSel", "HomeNet")
        await pg.fill("#wPass", "wrongpass1")
        await pg.click("#wConnect")
        await pg.wait_for_timeout(6500)
        print("wrong pw ->", (await pg.text_content("#netMsg")).strip()[:110])
        # correct password with a custom name
        await pg.fill("#wPass", "correct123")
        await pg.fill("#wHost", "cardio")
        await pg.click("#wConnect")
        await pg.wait_for_timeout(6500)
        print("correct ->", (await pg.text_content("#netMsg")).strip()[:220])
        await pg.screenshot(path="shot_wifi_ok.png")
        await pg.click("#netFix2")
        await pg.wait_for_timeout(800)
        print("fix ->", (await pg.text_content("#netMsg")).strip()[:90])
        print("chip:", await pg.text_content("#netChip"), "| card back in device section:",
              await pg.evaluate("() => document.getElementById('netHome').contains(document.getElementById('netCard'))"))
        # accelerometer calibration: 6 captures, then save
        for _ in range(6):
            await pg.click("#acalCap")
            await pg.wait_for_timeout(2400)
        print("acal:", await pg.text_content("#acalFaces"), "| save enabled:", await pg.is_enabled("#acalSave"), "|", await pg.text_content("#acalSave"))
        await pg.click("#acalSave")
        await pg.wait_for_timeout(600)
        print("acal saved:", await pg.text_content("#acalMsg"), "|", await pg.text_content("#acalState"))
        dev = await pg.text_content("#devSensor")
        page_text = await pg.evaluate("() => document.body.innerText")
        print("device card:", dev[:170])
        found = [bad for bad in ["MPU", "6050", "WHO_AM_I", "SDA", "SCL", "GPIO", "Wiring"] if bad in page_text]
        print("hardware details on page:", found if found else "none")
        print("banner:", await pg.text_content("#calTitle"), "|", (await pg.text_content("#calMsg"))[:90])
        await pg.click("#calHide")
        await pg.wait_for_timeout(1500)
        print("banner after hide:", await pg.get_attribute("#calBanner", "class"))
        await pg.evaluate("() => document.getElementById('deviceSec').scrollIntoView()")
        await pg.wait_for_timeout(500)
        await pg.screenshot(path="shot_device.png")
        print("errors:", errs if errs else "none")
        await b.close()

asyncio.run(main())
