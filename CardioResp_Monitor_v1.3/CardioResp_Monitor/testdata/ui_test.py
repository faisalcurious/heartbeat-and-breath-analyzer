import asyncio, json, sys, time
from playwright.async_api import async_playwright
CH = "/opt/pw-browsers/chromium-1194/chrome-linux/chrome"
async def run(vw, vh, name, wait_s, mobile):
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CH, args=["--no-sandbox"])
        ctx = await b.new_context(viewport={"width": vw, "height": vh}, device_scale_factor=2 if mobile else 1,
                                  is_mobile=mobile, has_touch=mobile, accept_downloads=True)
        pg = await ctx.new_page()
        errs = []
        pg.on("console", lambda m: errs.append(f"{m.type}: {m.text}") if m.type in ("error", "warning") else None)
        pg.on("pageerror", lambda e: errs.append(f"PAGEERROR: {e}"))
        await pg.goto("http://localhost:8080/")
        await pg.wait_for_timeout(wait_s * 1000)
        txt = await pg.evaluate("() => ({hr: document.getElementById('hrBig').textContent, rr: document.getElementById('rrBig').textContent, conn: document.getElementById('connTxt').textContent, ana: document.querySelectorAll('#analysis .finding').length, pattern: document.getElementById('patternTop').textContent, sqiH: document.getElementById('sqiH').textContent})")
        print(name, txt)
        await pg.screenshot(path=f"shot_{name}_top.png")
        await pg.screenshot(path=f"shot_{name}_full.png", full_page=True)
        # report workflow
        await pg.click("#btnLog")
        await pg.wait_for_timeout(1500)
        print(name, "logState:", await pg.text_content("#logState"))
        async with pg.expect_download() as dl:
            await pg.click("#btnPdf")
        d = await dl.value
        await d.save_as(f"ui_{name}.pdf")
        print(name, "downloaded", d.suggested_filename)
        async with pg.expect_download() as dl2:
            await pg.click("#btnCsv")
        d2 = await dl2.value
        await d2.save_as(f"ui_{name}.csv")
        # age command
        await pg.fill("#ageVal", "6"); await pg.select_option("#ageUnit", "m"); await pg.click("#ageApply")
        await pg.wait_for_timeout(500)
        print(name, "ageGroup:", await pg.text_content("#ageGroup"))
        await pg.click("#btnNight"); await pg.wait_for_timeout(600)
        await pg.screenshot(path=f"shot_{name}_night.png")
        await pg.click("#w300"); await pg.wait_for_timeout(500)
        await pg.screenshot(path=f"shot_{name}_night5.png")
        print(name, "errors:", errs if errs else "none")
        await b.close()
async def main():
    await run(390, 844, "mobile", 26, True)
    await run(1280, 900, "desktop", 14, False)
asyncio.run(main())
