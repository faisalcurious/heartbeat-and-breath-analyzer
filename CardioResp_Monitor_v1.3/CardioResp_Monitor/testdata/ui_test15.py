#!/usr/bin/env python3
"""End-to-end test of the v1.2 final dashboard against testdata/mock_device13.py (port 8081 / ws 81).
   Checks navigation (bottom bar / side bar), theme, typed alarms (with a fake audio device that records the tones),
   Live toolbar (capture, PDF, CSV, freeze, alarms), flip, calibration, recording PDFs, profiles, licence/lite mode."""
import asyncio, json, zipfile, urllib.request, sys, re, subprocess, os
from playwright.async_api import async_playwright
CH = "/opt/pw-browsers/chromium-1194/chrome-linux/chrome"
BASE = "http://localhost:8081"
BASE_DIR = __import__("os").path.dirname(__import__("os").path.dirname(__import__("os").path.abspath(__file__)))
OUT = "/tmp/ui15"; os.makedirs(OUT, exist_ok=True)
ok = True
def chk(name, cond, info=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name, "" if cond else info)
    ok = ok and bool(cond)
def get(p): return urllib.request.urlopen(BASE + p).read()
def calls(): return json.loads(urllib.request.urlopen(urllib.request.Request(BASE + "/api/calls", data=b"", method="POST")).read())
def pdftext(path): return subprocess.run(["pdftotext", "-layout", path, "-"], capture_output=True, text=True).stdout
def pdfpages(path):
    out = subprocess.run(["pdfinfo", path], capture_output=True, text=True).stdout
    m = re.search(r"Pages:\s+(\d+)", out); return int(m.group(1)) if m else 0
FAKE_AUDIO = """
window.__snd = [];
class FOsc { constructor(){ this.type='sine'; this.frequency = {setValueAtTime:(f)=>{ window.__snd.push(Math.round(f)); }, linearRampToValueAtTime(){} }; } connect(){} start(){} stop(){} }
class FGain { constructor(){ this.gain = {value:1, setValueAtTime(){}, linearRampToValueAtTime(){}}; } connect(){} }
class FCtx { constructor(){ this.state='running'; this.currentTime=0; this.destination={}; } resume(){} createOscillator(){ return new FOsc(); } createGain(){ return new FGain(); } }
window.AudioContext = FCtx; window.webkitAudioContext = FCtx;
"""
async def main():
    get("/api/scn?v=normal"); get("/api/set?k=apc&v=0"); get("/api/set?k=mot&v=0")
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path=CH, args=["--no-sandbox"])
        # ---------------- desktop: side bar ----------------
        dctx = await b.new_context(viewport={"width": 1366, "height": 900})
        dp = await dctx.new_page(); await dp.goto(BASE + "/"); await dp.wait_for_timeout(1500)
        bb = await dp.locator("nav.tabs").bounding_box()
        chk("desktop: navigation is a left side bar", bb["x"] == 0 and bb["height"] >= 880 and bb["width"] < 260, bb)
        await dctx.close()
        # ---------------- phone ----------------
        ctx = await b.new_context(viewport={"width": 400, "height": 860}, is_mobile=True, has_touch=True, accept_downloads=True)
        await ctx.add_init_script(FAKE_AUDIO)
        pg = await ctx.new_page(); errs = []; bad = []
        pg.on("console", lambda m: errs.append(f"{m.type}: {m.text}") if m.type in ("error", "warning") else None)
        pg.on("pageerror", lambda e: errs.append(f"PAGEERROR: {e.message} {(e.stack or '')[:400]}"))
        pg.on("response", lambda r: bad.append(r.url) if r.status >= 400 else None)
        pg.on("dialog", lambda d: asyncio.ensure_future(d.accept()))
        await pg.goto(BASE + "/"); await pg.wait_for_timeout(2500)
        T = lambda i: pg.text_content("#" + i)
        bb = await pg.locator("nav.tabs").bounding_box()
        chk("phone: navigation is a bottom bar", bb["y"] + bb["height"] >= 855 and bb["width"] >= 395 and bb["height"] < 90, bb)
        chk("5 nav items", await pg.locator(".tabs a").count() == 5)
        await pg.mouse.wheel(0, 3000); await pg.wait_for_timeout(300)
        bb2 = await pg.locator("nav.tabs").bounding_box()
        chk("bottom bar stays visible when scrolled", abs(bb2["y"] - bb["y"]) < 2, (bb, bb2))
        await pg.mouse.wheel(0, -3000)
        chk("first start in direct mode: Wi-Fi set-up offered on top", await pg.evaluate("document.getElementById('netTop').contains(document.getElementById('netCard'))"))
        await pg.click("#wSkip"); await pg.wait_for_timeout(300)
        chk("'Not now' moves the Wi-Fi card to Settings", await pg.evaluate("document.getElementById('netHome').contains(document.getElementById('netCard'))"))
        await pg.click("body")                                  # one tap: unlocks audio like on a phone
        # pages
        for name in ["people", "mesh", "files", "settings", "live"]:
            await pg.click(f'.tabs a[data-p="{name}"]'); await pg.wait_for_timeout(350)
            chk("page " + name, await pg.is_visible("#pg-" + name) and await pg.get_attribute(f'.tabs a[data-p="{name}"]', "aria-current") == "page")
        # live layout
        chk("live toolbar: capture / pdf / csv / freeze / alarms", all([await pg.is_visible("#" + i) for i in ["btnLog", "btnPdf", "btnCsv", "btnPause", "btnSound"]]))
        chk("flip button in the breathing card", await pg.locator("article.vital.resp #btnPol").count() == 1)
        chk("2-D placement graph shown", await pg.is_visible("#cvLevel") and "Sensor placement" in await T("orientSec"))
        chk("3-D probe removed", await pg.locator("#cvProbe").count() == 0)
        chk("analysis collapsed by default", not await pg.evaluate("document.getElementById('anaDrop').open") and not await pg.is_visible("#cvTrend"))
        await pg.click("#anaDrop > summary"); await pg.wait_for_timeout(600)
        chk("analysis opens", await pg.is_visible("#cvTrend") and await pg.is_visible("#analysis"))
        trend_px = await pg.evaluate("(()=>{const c=document.getElementById('cvTrend');return c.width*c.height})()")
        chk("charts drawn after opening", trend_px > 1000, trend_px)
        await pg.click("#anaDrop > summary"); await pg.wait_for_timeout(200)
        await pg.goto(BASE + "/#analysis"); await pg.wait_for_timeout(1500)
        chk("old #analysis link opens the drop-down", await pg.evaluate("document.getElementById('anaDrop').open") and await pg.is_visible("#pg-live"))
        await pg.goto(BASE + "/#about"); await pg.wait_for_timeout(1200)
        chk("old #about -> settings", await pg.is_visible("#pg-settings") and await pg.is_visible("#abName"))
        await pg.goto(BASE + "/#nurse"); await pg.wait_for_timeout(1200)
        chk("old #nurse -> people", await pg.is_visible("#nuName") and await pg.is_visible("#dpName"))
        await pg.goto(BASE + "/#live"); await pg.wait_for_timeout(2000); await pg.click("body")
        # theme
        await pg.click('.tabs a[data-p="settings"]'); await pg.wait_for_timeout(300)
        await pg.click('#themeSeg button[data-th="dark"]'); await pg.wait_for_timeout(200)
        chk("dark theme", await pg.evaluate("document.documentElement.dataset.theme") == "night")
        bg = await pg.evaluate("getComputedStyle(document.body).backgroundColor")
        chk("dark background", bg in ("rgb(16, 22, 29)",), bg)
        await pg.screenshot(path=f"{OUT}/settings_dark.png")
        await pg.reload(); await pg.wait_for_timeout(1500)
        chk("theme remembered after reload", await pg.evaluate("document.documentElement.dataset.theme") == "night" and await pg.get_attribute('#themeSeg button[data-th="dark"]', "aria-pressed") == "true")
        await pg.click('#themeSeg button[data-th="light"]'); await pg.wait_for_timeout(200)
        chk("light theme", await pg.evaluate("document.documentElement.dataset.theme") == "day")
        await pg.click('#themeSeg button[data-th="auto"]')
        # alarm settings table
        chk("alarm table lists 8 types", await pg.locator("#alTab tbody tr").count() == 8)
        await pg.evaluate("window.__snd = []")
        await pg.click('#alTab button[data-test="apnea"]'); await pg.wait_for_timeout(200)
        chk("test button plays the breathing-pause siren", await pg.evaluate("window.__snd") == [880, 587, 880, 587], await pg.evaluate("window.__snd"))
        await pg.evaluate("window.__snd = []")
        await pg.click('#alTab button[data-test="heart"]'); await pg.wait_for_timeout(200)
        chk("heart sound = three equal beeps", await pg.evaluate("window.__snd") == [784, 784, 784], await pg.evaluate("window.__snd"))
        # condition mapping (unit level)
        cond = await pg.evaluate("""(()=>{const P=ageProfile(30);const S={cal:2,hr:132,pres:1,sqh:80,rr:30,sqr:80,apc:25,cgh:6,vra:150,tb:38.6,temp:31.0};
            const c=CRAlarm.conditions(S,{P,hrZ:2,zone:CR.zone,tz:2,tb:38.6,vz:{a:3,pk:0,cg:2},v:{sev:3,code:"arrest",head:"Heart signal lost for 12 s"},full:true});
            return Object.fromEntries(Object.entries(c).map(([k,v])=>[k,v.p]));})()""")
        chk("conditions: every type maps with its priority", cond == {"cardiac": 3, "apnea": 3, "heart": 2, "breath": 2, "vib": 1, "temp": 1}, cond)
        cond4 = await pg.evaluate("""(()=>{const P=ageProfile(30);return Object.keys(CRAlarm.conditions({cal:2,tb:31.2,temp:31.2,mot:1,vra:150,cgh:0},{P,hrZ:0,zone:CR.zone,tz:3,tb:31.2,vz:{a:3,pk:3,cg:0},v:null,full:true}));})()""")
        chk("no temperature alarm before calibration, no vibration alarm during movement", cond4 == [], cond4)
        cond2 = await pg.evaluate("""(()=>{const P=ageProfile(30);return Object.keys(CRAlarm.conditions({cal:3,calmsg:"Sensor connection lost"},{P,hrZ:0,zone:CR.zone,tz:0,tb:NaN,vz:null,v:null,full:true}));})()""")
        chk("sensor lost -> sensor alarm", cond2 == ["sensor"], cond2)
        c5 = await pg.evaluate("""(()=>{const P=ageProfile(30);const c=CRAlarm.conditions({cal:2,apc:12,rr1:40,rr:16,sqr:80,hr:200,pres:1,sqh:90},{P,zone:CR.zone,tz:0,tb:NaN,vz:null,v:null,full:false});return Object.fromEntries(Object.entries(c).map(([k,v])=>[k,v.p]));})()""")
        chk("short pause = warning, last-minute breathing rate used, far-out heart rate = urgent, works without licence", c5 == {"apnea": 2, "breath": 3, "heart": 3}, c5)
        c6 = await pg.evaluate("""(()=>{const P=ageProfile(30);return Object.keys(CRAlarm.conditions({cal:2,hr:160,pres:1,sqh:20,rr:16,sqr:80},{P,zone:CR.zone,tz:0,tb:NaN,vz:null,v:{sev:3,code:"tachy",head:"x"},full:true}));})()""")
        chk("assessment cannot bypass the signal-quality check", c6 == [], c6)
        inter = await pg.evaluate("""(async()=>{
            const P=ageProfile(30), real=Date.now; let t=real.call(Date)+1e6; Date.now=()=>t; const out={};
            const st=CRAlarm.state; st.act.clear(); st.pend.clear(); st.miss.clear();
            for(let i=0;i<40;i++){ const bad = i%4===0; CRAlarm.update({cal:2,hr:bad?132:75,pres:1,sqh:80,rr:16,sqr:80,temp:31,tb:31}, {P,zone:CR.zone,tz:0,tb:NaN,vz:null,v:null,full:true}); t+=1000; }
            out.inter = st.act.has("heart");
            for(let i=0;i<14;i++){ const dip = i%8===7; CRAlarm.update({cal:2,hr:132,pres:1,sqh:dip?20:80,rr:16,sqr:80,temp:31,tb:31}, {P,zone:CR.zone,tz:0,tb:NaN,vz:null,v:null,full:true}); t+=1000; }
            out.steady = st.act.has("heart");
            for(let i=0;i<5;i++){ CRAlarm.update({cal:2,hr:75,pres:1,sqh:80,rr:16,sqr:80,temp:31,tb:31}, {P,zone:CR.zone,tz:0,tb:NaN,vz:null,v:null,full:true}); t+=1000; }
            out.cleared = !st.act.has("heart"); Date.now=real;
            st.act.clear(); st.pend.clear(); st.miss.clear(); for (const k in st.last) delete st.last[k]; st.statusAt=Date.now(); st.lastVib=0; st.dt=1000; st.lastS=null;
            return out; })()""")
        chk("hold time: 1 s in 4 s gives no alarm; steady with a 1 s dip does; it clears afterwards", inter == {"inter": False, "steady": True, "cleared": True}, inter)
        chk("safety alarms cannot be switched off", all([await pg.is_disabled(f'#alTab input[data-k="{k}"]') for k in ["cardiac", "apnea", "sensor", "conn"]]) and not await pg.is_disabled('#alTab input[data-k="temp"]'))
        cond3 = await pg.evaluate("""(()=>{const P=ageProfile(30);return Object.keys(CRAlarm.conditions({cal:2,hr:132,pres:1,sqh:20,rr:16,sqr:80},{P,hrZ:2,zone:CR.zone,tz:0,tb:36.8,vz:null,v:null,full:true}));})()""")
        chk("poor heart signal does not raise a heart alarm", cond3 == [], cond3)
        # live alarms through the mock
        await pg.click('.tabs a[data-p="live"]'); await pg.wait_for_timeout(300)
        await pg.evaluate("window.__snd = []")
        get("/api/scn?v=tachy"); await pg.wait_for_timeout(5000)
        chk("heart alarm waits for the 10 s hold", await pg.is_hidden("#alarmBar") or "Heart rate" not in (await T("alarmList")), await T("alarmList"))
        await pg.wait_for_timeout(7500)
        txt = await T("alarmList")
        chk("heart alarm active after 10 s", "Heart rate or rhythm" in txt and "132 bpm" in txt, txt)
        chk("alarm bar colour = warning", "p2" in await pg.get_attribute("#alarmBar", "class") or "p3" in await pg.get_attribute("#alarmBar", "class"))
        chk("heart card highlighted", "alarming" in await pg.get_attribute("article.vital.heart", "class"))
        snd = await pg.evaluate("window.__snd")
        chk("heart alarm sounded (784 Hz beeps)", "784,784,784" in ",".join(map(str, snd)), snd)
        chk("cough notice sounded first (rising chirps 450 Hz)", snd[:2] == [450, 450], snd)
        await pg.screenshot(path=f"{OUT}/alarm_heart.png")
        # silence, then an urgent alarm breaks through
        await pg.click("#alarmSil"); await pg.wait_for_timeout(300)
        chk("silence button", "Silenced" in await T("alarmSil"))
        await pg.evaluate("window.__snd = []"); await pg.wait_for_timeout(11000)
        snd = await pg.evaluate("window.__snd")
        chk("silenced: the heart warning does not repeat", "784" not in ",".join(map(str, snd)), snd)
        chk("a new alarm during the silence still sounds (temperature notice)", snd in ([], [523, 392]), snd)
        get("/api/set?k=apc&v=25"); await pg.wait_for_timeout(2500)
        snd = await pg.evaluate("window.__snd")
        chk("new urgent alarm (breathing pause) sounds through the silence", "880,587,880,587" in ",".join(map(str, snd)), snd)
        chk("alarm bar urgent", "p3" in await pg.get_attribute("#alarmBar", "class") and "Breathing pause" in await T("alarmList"))
        chk("while an alarm breaks through, the button offers Silence (not undo)", await T("alarmSil") == "Silence 2 min", await T("alarmSil"))
        await pg.click("#alarmSil"); await pg.wait_for_timeout(300)
        chk("pressing it silences the new alarm too", "Silenced" in await T("alarmSil") and await pg.evaluate("CRAlarm.state.sil.get('apnea')") == 3, await T("alarmSil"))
        chk("live nav badge", "alarm" in await pg.get_attribute('.tabs a[data-p="live"]', "class"))
        await pg.screenshot(path=f"{OUT}/alarm_apnea.png")
        get("/api/set?k=apc&v=0"); await pg.wait_for_timeout(4500)             # pause ends (3 s grace)
        chk("pause cleared", "Breathing pause" not in await T("alarmList"), await T("alarmList"))
        await pg.evaluate("window.__snd = []"); get("/api/set?k=apc&v=25"); await pg.wait_for_timeout(2000)
        chk("a returning pause sounds again during the silence", (await pg.evaluate("window.__snd"))[:4] == [880, 587, 880, 587], await pg.evaluate("window.__snd"))
        get("/api/set?k=apc&v=0"); await pg.wait_for_timeout(4500)
        await pg.evaluate("window.__snd = []"); get("/api/set?k=hrx&v=170"); await pg.wait_for_timeout(2500)
        chk("heart warning that becomes urgent sounds through the silence", "784,784,784" in ",".join(map(str, await pg.evaluate("window.__snd"))) and "p3" in await pg.get_attribute("#alarmBar", "class"), await pg.evaluate("window.__snd"))
        get("/api/set?k=hrx&v=0"); await pg.wait_for_timeout(1500)
        await pg.click("#alarmSil"); await pg.wait_for_timeout(200)       # undo silence (a second press if the first re-silenced a new alarm)
        if await pg.evaluate("CRAlarm.state.silUntil") > 0: await pg.click("#alarmSil"); await pg.wait_for_timeout(200)
        chk("silence undone", await T("alarmSil") == "Silence 2 min" and await pg.evaluate("CRAlarm.state.silUntil") == 0, await T("alarmSil"))
        # alarms off from the toolbar
        await pg.click("#btnSound"); await pg.wait_for_timeout(200)
        chk("toolbar alarm toggle", await pg.get_attribute("#btnSound", "aria-pressed") == "false" and "off" in await T("btnSound"))
        await pg.evaluate("window.__snd = []"); await pg.wait_for_timeout(5000)
        chk("alarms off: silent", await pg.evaluate("window.__snd") == [], await pg.evaluate("window.__snd"))
        await pg.click('.tabs a[data-p="settings"]'); await pg.wait_for_timeout(200)
        chk("settings checkbox follows the toolbar", not await pg.is_checked("#alOn"))
        await pg.reload(); await pg.wait_for_timeout(1500)
        chk("switching all alarms off is not remembered after reopening", await pg.get_attribute("#btnSound", "aria-pressed") == "true")
        await pg.click('.tabs a[data-p="live"]'); await pg.click("body")
        get("/api/set?k=apc&v=0"); get("/api/scn?v=normal"); await pg.wait_for_timeout(2500)
        chk("alarms clear when values recover", await pg.is_hidden("#alarmBar"), await T("alarmList"))
        # flip, freeze
        await pg.click("#btnPol"); await pg.wait_for_timeout(400)
        chk("flip sends the polarity command", any(c[0] == "ws" and '"polarity"' in c[1] for c in calls()))
        await pg.click("#btnPause"); chk("freeze", await T("btnPause") == "Resume" and await pg.get_attribute("#btnPause", "aria-pressed") == "true")
        await pg.click("#btnPause")
        # capture and PDF on the live page
        await pg.click("#btnLog"); await pg.wait_for_timeout(2500)
        chk("capture done", "Captured" in await T("logState") and not await pg.is_disabled("#btnPdf"), await T("logState"))
        chk("captured report shown on live", await pg.is_visible("#reportBox") and await pg.is_visible("#pg-live"))
        async with pg.expect_download() as dl: await pg.click("#btnPdf")
        d = await dl.value; path = f"{OUT}/live_report.pdf"; await d.save_as(path)
        chk("live PDF valid", open(path, "rb").read(5) == b"%PDF-" and pdfpages(path) >= 3 and "Cardio-respiratory recording" in pdftext(path), pdfpages(path))
        async with pg.expect_download() as dl: await pg.click("#btnCsv")
        d = await dl.value; chk("live CSV", d.suggested_filename.endswith(".csv"))
        await pg.screenshot(path=f"{OUT}/live_top.png")
        # settings: calibration
        await pg.click('.tabs a[data-p="settings"]'); await pg.wait_for_timeout(300)
        await pg.click("#btnRecal"); await pg.wait_for_timeout(400)
        chk("recalibrate from settings", any(c[0] == "ws" and '"recal"' in c[1] for c in calls()))
        await pg.click("#btnReset"); await pg.wait_for_timeout(400)
        chk("restart measurement from settings", any(c[0] == "ws" and '"reset"' in c[1] for c in calls()))
        chk("calibration cards grouped", all([await pg.locator("#calSec #" + i).count() == 1 for i in ["calCard", "acalCard", "tcCard"]]))
        await pg.select_option("#pwSel", "2"); await pg.click("#pwSave"); await pg.wait_for_timeout(500)
        chk("power mode eco", "Eco" in await T("pwMsg"), await T("pwMsg"))
        # files: PDF of a stored recording
        await pg.click('.tabs a[data-p="files"]'); await pg.wait_for_timeout(800)
        chk("3 recordings listed with PDF links", await pg.locator("#fileTab tbody tr").count() == 3 and await pg.locator("#fileTab a[data-pdf]").count() == 3)
        async with pg.expect_download() as dl: await pg.click('#fileTab a[data-pdf="rec0003.crr"]')
        d = await dl.value; path = f"{OUT}/rec0003_report.pdf"; await d.save_as(path)
        t = pdftext(path)
        chk("recording PDF: valid, 2 pages", open(path, "rb").read(5) == b"%PDF-" and pdfpages(path) == 2, pdfpages(path))
        chk("recording PDF: content", all(s in t for s in ["Recording results report", "A. Patient", "N. Nurse", "Heart rate (bpm)", "Findings", "Waveforms", "SCG"]), t[:600])
        chk("recording PDF: abnormal episode found", "Warning" in t and ("above the reference" in t or "outside the reference range" in t), t[:1500])
        async with pg.expect_download() as dl: await pg.click("#filePdfAll")
        d = await dl.value; path = f"{OUT}/all_report.pdf"; await d.save_as(path)
        chk("PDF of all recordings", pdfpages(path) >= 4 and pdftext(path).count("Recording results report") == 3, pdfpages(path))
        async with pg.expect_download() as dl: await pg.click('#fileTab a[data-csv="rec0001.crr"]')
        d = await dl.value; txt = open(await d.path()).read()
        chk("csv content", "second,hr_bpm" in txt and "36.50" in txt, txt[:200])
        async with pg.expect_download() as dl: await pg.click("#fileAll")
        d = await dl.value; z = zipfile.ZipFile(await d.path()); chk("zip valid", z.testzip() is None and len(z.namelist()) == 3, z.namelist())
        # profiles
        await pg.click('.tabs a[data-p="people"]'); await pg.wait_for_timeout(500)
        chk("patient + nurse on one page", await pg.input_value("#dpName") == "A. Patient" and await pg.input_value("#nuWard") == "ICU")
        # lite mode
        await pg.click('.tabs a[data-p="settings"]'); await pg.click("#licDel"); await pg.wait_for_timeout(2500)
        await pg.click('.tabs a[data-p="live"]'); await pg.wait_for_timeout(500)
        chk("lite: placement graph hidden, rates shown", not await pg.is_visible("#orientSec") and await pg.is_visible("#hrBig"))
        await pg.click('.tabs a[data-p="settings"]'); await pg.fill("#licTok", "good-token-" + "x" * 40); await pg.click("#licAct"); await pg.wait_for_timeout(2500)
        chk("reactivated", not await pg.evaluate("document.body.classList.contains('lite')"))
        # dark-mode live screenshot for the record
        await pg.click('#themeSeg button[data-th="dark"]'); await pg.click('.tabs a[data-p="live"]'); await pg.wait_for_timeout(1500)
        await pg.screenshot(path=f"{OUT}/live_dark.png")
        await pg.click('.tabs a[data-p="settings"]'); await pg.click('#themeSeg button[data-th="auto"]')
        # ---------------- v1.2 final round: analysis order, tools, event log, closable notes, mesh settings ----------------
        dist = open(BASE_DIR + "/web/dist.html").read()
        import re as _re
        bad_words = [w for w in _re.findall(r"(?i)esp[- _]?32\w*|mpu[- _]?6050|mpu-?\d{4}|super ?mini|gy-?521|espressif|esp-now", dist)]
        chk("page source has no controller / sensor model names", not bad_words, bad_words[:5])
        await pg.click('.tabs a[data-p="live"]'); await pg.wait_for_timeout(300)
        if not await pg.evaluate("document.getElementById('anaDrop').open"): await pg.click("#anaDrop > summary")
        await pg.wait_for_timeout(2500)
        order = await pg.evaluate("""(()=>{const y=id=>document.getElementById(id).getBoundingClientRect().top;return [y('cvTrend'),y('cvEns'),y('cvSpecC'),y('analysis'),y('evTab')]})()""")
        chk("analysis: graphs above, text below", order == sorted(order), order)
        chk("average heartbeat tool", "beats averaged" in await T("ensTxt"), await T("ensTxt"))
        chk("HRV statistics", "SDNN" in await T("hrvStats") and "pNN50" in await T("hrvStats"), await T("hrvStats"))
        chk("breath-by-breath tool", "Mean" in await T("bbbTxt") and "/min" in await T("bbbTxt"), await T("bbbTxt"))
        await pg.click('#trWin button[data-w="600"]'); await pg.wait_for_timeout(200)
        chk("trend window selector", await pg.get_attribute('#trWin button[data-w="600"]', "aria-pressed") == "true")
        await pg.click('#trWin button[data-w="0"]')
        # closable notifications
        await pg.evaluate("window.scrollTo(0,0)")
        if await pg.is_visible("#licBanner"):
            await pg.click("#licBanner [data-close]"); await pg.wait_for_timeout(500)
            chk("licence notice can be closed", not await pg.is_visible("#licBanner"))
        get("/api/set?k=apc&v=25"); await pg.wait_for_timeout(2500)
        chk("breathing-pause notice shown", await pg.is_visible("#alertBanner") and "Breathing pause" in await T("alertTitle"), await T("alertTitle"))
        await pg.click("#alertBanner [data-close]"); await pg.wait_for_timeout(1500)
        chk("alert notice can be closed and stays closed", not await pg.is_visible("#alertBanner"))
        get("/api/set?k=apc&v=0"); get("/api/set?k=mot&v=1"); await pg.wait_for_timeout(2500)
        chk("a different alert message shows again", await pg.is_visible("#alertBanner") and "Movement" in await T("alertTitle"), await T("alertTitle"))
        get("/api/set?k=mot&v=0")
        get("/api/set?k=apc&v=25"); await pg.wait_for_timeout(2000)
        chk("alarm bar visible", await pg.is_visible("#alarmBar"))
        await pg.click("#alarmX"); await pg.wait_for_timeout(600)
        chk("x on the alarm bar hides it and silences", not await pg.is_visible("#alarmBar") and await pg.evaluate("CRAlarm.state.silUntil") > 0)
        get("/api/set?k=hrx&v=170"); await pg.wait_for_timeout(12000)
        chk("a new urgent alarm brings the bar back", await pg.is_visible("#alarmBar") and "Heart" in await T("alarmList"), await T("alarmList"))
        get("/api/set?k=hrx&v=0"); get("/api/set?k=apc&v=0"); await pg.wait_for_timeout(4500)
        if await pg.evaluate("CRAlarm.state.silUntil") > 0: await pg.evaluate("CRAlarm.state.silUntil = 0; CRAlarm.state.hideUntil = 0; CRAlarm.state.sil.clear()")
        if not await pg.evaluate("document.getElementById('anaDrop').open"): await pg.click("#anaDrop > summary")
        await pg.wait_for_timeout(1500)
        chk("event log lists the alarms of this page session", await pg.locator("#evTab tbody tr").count() >= 2 and "Breathing pause" in await T("evTab") and "Heart rate" in await T("evTab"), await T("evTab"))
        async with pg.expect_download() as dl: await pg.click("#evCsv")
        d = await dl.value; txt = open(await d.path()).read()
        chk("event log CSV", txt.startswith("start,end,duration_s,alarm,level,details") and "Breathing pause" in txt and "urgent" in txt, txt[:300])
        # mesh settings and verification
        await pg.click('.tabs a[data-p="settings"]'); await pg.wait_for_timeout(1200)
        chk("mesh settings loaded", await pg.input_value("#mgGrp") == "ward" and "unchanged" in (await pg.get_attribute("#mgPin", "placeholder")) and "set (4 digits)" in await T("mgInfo"), await T("mgInfo"))
        await pg.fill("#mgPin", "12a"); await pg.click("#mgSave"); await pg.wait_for_timeout(300)
        chk("invalid PIN refused", "4 to 8 digits" in await T("mgMsg"), await T("mgMsg"))
        await pg.fill("#mgGrp", "bad/name"); await pg.fill("#mgPin", ""); await pg.click("#mgSave"); await pg.wait_for_timeout(300)
        chk("invalid group refused", "Group name" in await T("mgMsg"), await T("mgMsg"))
        await pg.fill("#mgGrp", "ward"); await pg.fill("#mgPin", "1111"); await pg.fill("#mgLbl", "Bed 4"); await pg.select_option("#mgPer", "5000"); await pg.select_option("#mgTtl", "1")
        await pg.click("#mgSave"); await pg.wait_for_timeout(600)
        chk("mesh settings saved", "Saved" in await T("mgMsg") and await pg.input_value("#mgPer") == "5000" and await pg.input_value("#mgTtl") == "1", await T("mgMsg"))
        await pg.click("#mgVerify"); await pg.wait_for_timeout(4500)
        vt = await T("mgTab")
        chk("verify with a different PIN: ward monitors listed as wrong PIN", "Bed 7" in vt and "wrong PIN" in vt and "verified" not in vt.replace("not verified", ""), vt)
        await pg.fill("#mgPin", "4821"); await pg.click("#mgSave"); await pg.wait_for_timeout(600)
        await pg.click("#mgVerify"); await pg.wait_for_timeout(4500)
        vt = await T("mgTab")
        chk("verify with the ward PIN: verified, plus other group / wrong PIN / older firmware", all(w in vt for w in ["verified", "other group", "wrong PIN", "older software"]) and "2 verified" in await T("mgVMsg"), vt)
        await pg.click('.tabs a[data-p="mesh"]'); await pg.wait_for_timeout(800)
        chk("mesh page: group shown and unverified count", "ward" in await T("meshSub") and "not verified" in await T("meshInfo"), await T("meshSub"))
        await pg.click('.tabs a[data-p="settings"]'); await pg.wait_for_timeout(800)
        await pg.check("#mgOpen"); await pg.click("#mgSave"); await pg.wait_for_timeout(600)
        chk("open mesh (no PIN)", "open mesh" in await T("mgInfo"), await T("mgInfo"))
        await pg.uncheck("#mgOpen"); await pg.fill("#mgPin", "4821"); await pg.select_option("#mgPer", "2000"); await pg.select_option("#mgTtl", "2"); await pg.click("#mgSave"); await pg.wait_for_timeout(600)
        chk("PIN restored", "set (4 digits)" in await T("mgInfo"), await T("mgInfo"))
        for name in ["live", "people", "mesh", "files", "settings"]:
            await pg.click(f'.tabs a[data-p="{name}"]'); await pg.wait_for_timeout(500)
            sw = await pg.evaluate("[document.documentElement.scrollWidth, innerWidth]")
            chk(f"no sideways overflow on the phone: {name}", sw[0] <= 400 and sw[1] <= 400, sw)
        # ---------------- sensor lost and no data (WebSocket intercepted) ----------------
        ctx2 = await b.new_context(viewport={"width": 400, "height": 860}, is_mobile=True, has_touch=True)
        await ctx2.add_init_script(FAKE_AUDIO.replace("this.state='running';", "this.state='suspended';").replace("resume(){}", "resume(){ if (window.__allow) this.state='running'; return Promise.resolve(); }"))
        await ctx2.add_init_script("try{localStorage.setItem('cr_wifi_skip','1')}catch(e){}")
        q = await ctx2.new_page(); state = {"mode": "pass"}
        q.on("pageerror", lambda e: errs.append(f"PAGEERROR2: {e.message}"))
        LOST = {"t": "s", "sid": 1, "up": 99000, "cal": 3, "calp": 0, "cala": 1, "age": 35, "full": 1,
                "calmsg": "Sensor connection lost (SDA 8 / SCL 9) - check wiring; it restarts automatically", "bat": {"ok": 1, "v": 3.9, "p": 66}, "temp": 31.0}
        def route(ws):
            server = ws.connect_to_server()
            def fwd(m):
                if state["mode"] == "drop": return
                if state["mode"] == "lost" and '"t": "s"' in m: ws.send(json.dumps(LOST)); return
                ws.send(m)
            server.on_message(fwd)
        await q.route_web_socket("**", route)
        await q.goto(BASE + "/"); await q.wait_for_timeout(2500)
        chk("normal stream: no alarm", await q.is_hidden("#alarmBar"))
        state["mode"] = "lost"; await q.wait_for_timeout(5000)
        chk("sensor lost: vitals blanked", "--" in await q.text_content("#hrBig") and "Sensor connection lost" in await q.text_content("#calTitle"))
        chk("sensor lost: sensor alarm", "Sensor lost" in await q.text_content("#alarmList") and not await q.is_hidden("#alarmBar"), await q.text_content("#alarmList"))
        chk("before the first tap: no queued sound, and the bar asks for a tap", await q.evaluate("window.__snd") == [] and "Tap the screen once" in await q.text_content("#alarmList"), await q.evaluate("window.__snd"))
        await q.evaluate("window.__allow = true"); await q.click("body"); await q.wait_for_timeout(1200)
        snd = await q.evaluate("window.__snd")
        chk("after the tap: one sensor sound (two low buzzes), no pile-up", snd == [330, 330], snd)
        await q.screenshot(path=f"{OUT}/alarm_sensor.png")
        state["mode"] = "pass"; await q.wait_for_timeout(4500)
        chk("sensor back: alarm clears", await q.is_hidden("#alarmBar"), await q.text_content("#alarmList"))
        state["mode"] = "drop"; await q.wait_for_timeout(18000)
        chk("no data for 15 s: connection alarm", "no data from the monitor" in await q.text_content("#alarmList"), await q.text_content("#alarmList"))
        await q.evaluate("CR.applyTheme('dark')"); await q.wait_for_timeout(700)
        chk("changing the theme does not clear the connection alarm", "no data from the monitor" in await q.text_content("#alarmList"), await q.text_content("#alarmList"))
        await q.evaluate("CR.applyTheme('auto')")
        state["mode"] = "pass"; await q.wait_for_timeout(4000)
        chk("data back: connection alarm clears", await q.is_hidden("#alarmBar"), await q.text_content("#alarmList"))
        await ctx2.close()
        real = [e for e in errs if "favicon" not in e and "Failed to load resource" not in e]
        chk("no console errors", not real, real)
        bad2 = [u for u in bad if "favicon" not in u and "/api/wifi/scan" not in u]
        chk("no failed requests", not bad2, bad2)
        await b.close()
asyncio.run(main()); print("ALL PASS" if ok else "SOME FAILED"); sys.exit(0 if ok else 1)
