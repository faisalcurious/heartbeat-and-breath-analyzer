# Injects a "sensor connection lost" status into a live session and checks the page blanks the vitals.
import asyncio, json
from playwright.async_api import async_playwright
CH = "/opt/pw-browsers/chromium-1194/chrome-linux/chrome"
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CH, args=["--no-sandbox"])
        pg = await b.new_page(viewport={"width": 390, "height": 844})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        holder = {}
        async def route(ws):
            server = ws.connect_to_server()
            holder["ws"] = ws
        await pg.route_web_socket("**", route)
        await pg.goto("http://localhost:8080/")
        await pg.wait_for_timeout(9000)
        q = "() => [document.getElementById('hrBig').textContent, document.getElementById('rrBig').textContent, document.getElementById('calTitle').textContent, document.getElementById('calBanner').className]"
        print("live :", await pg.evaluate(q))
        lost = {"t": "s", "sid": 1, "up": 99000, "cal": 3, "calp": 0, "cala": 1, "age": 35,
                "calmsg": "Sensor connection lost (SDA 6 / SCL 7) - check wiring; it restarts automatically",
                "bat": {"ok": 1, "v": 3.9, "p": 66}, "temp": 31.0}
        # keep injecting (the replay keeps sending normal status messages in between)
        for _ in range(3):
            holder["ws"].send(json.dumps(lost)); await pg.wait_for_timeout(60)
            print("lost :", await pg.evaluate(q))
        print("errors:", errs or "none")
        await b.close()
asyncio.run(main())
