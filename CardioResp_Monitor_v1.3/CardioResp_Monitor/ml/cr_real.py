"""
cr_real.py - real-data helpers for the Colab notebook (ml/CardioResp_Colab.ipynb).

  ensure_physionet()   download a PhysioNet database ONCE into Google Drive (marker file = cache)
  status_from_beats()  one analysis window (beat times [+ breathing signal]) -> the same "status" dict the
                       monitor streams, so cr_features.features() gives the 21 model inputs
  label_window()       weak labels from annotations + age-referenced limits
  load_user_bank()     your own table (CSV) with status columns + label
  load_device_csv()    CSV exported from the monitor's Files page

HONEST LIMITS: reference ECG/respiration gives rate and variability, but not the chest-vibration quality
indices (sqh, sqr, vibration high-frequency share, cough bursts ...).  Fields a dataset cannot provide stay
None; features() then uses its neutral defaults.  Cough (class 6) has no public labelled SCG source here:
keep the synthetic samples for it, or add your own labelled bank.
"""
import os, json, math, glob
import numpy as np
from cr_features import AGE_TABLE, CLASS_NAMES, features, age_profile

PHYSIONET = {   # name: (database id, what it provides)
    "cebs":  ("cebsdb",   "ECG + breathing + seismocardiogram, 20 healthy adults (CEBS)"),
    "afdb":  ("afdb",     "MIT-BIH Atrial Fibrillation DB: AF / normal rhythm annotations"),
    "apnea": ("apnea-ecg", "Apnea-ECG DB: per-minute apnoea annotations (A / N)"),
    "bidmc": ("bidmc",    "BIDMC: ECG, PPG, respiration, 53 ICU patients"),
    "mitdb": ("mitdb",    "MIT-BIH Arrhythmia DB"),
}
CLS = {n: i for i, n in enumerate(CLASS_NAMES)}


def ensure_physionet(key, root):
    """Download once. A marker file records success, so later runs (even next day) skip the download."""
    import wfdb
    db, desc = PHYSIONET[key]
    dest = os.path.join(root, "datasets", key)
    mark = os.path.join(dest, ".complete")
    if os.path.exists(mark):
        print(f"[cache] {key}: already in Drive ({dest})"); return dest
    os.makedirs(dest, exist_ok=True)
    print(f"[download] {key}: {desc}")
    wfdb.dl_database(db, dest)
    open(mark, "w").write(json.dumps({"db": db, "files": len(os.listdir(dest))}))
    return dest


def _fin(v): return v is not None and isinstance(v, (int, float)) and math.isfinite(v)


def status_from_beats(beat_t, resp=None, fs_resp=None, t0=None, t1=None, quality=80):
    """beat_t: beat times in seconds (numpy). resp: breathing signal (optional) sampled at fs_resp.
    Window = [t0, t1). Returns a status dict or None when fewer than 8 beats."""
    b = np.asarray(beat_t, float)
    if t0 is None: t0 = b[0]
    if t1 is None: t1 = b[-1]
    b = b[(b >= t0) & (b < t1)]
    if len(b) < 8: return None
    ibi = np.diff(b) * 1000.0
    ibi = ibi[(ibi > 250) & (ibi < 2000)]
    if len(ibi) < 6: return None
    nn = float(np.mean(ibi)); d = np.diff(ibi)
    inst = 60000.0 / ibi
    S = dict(hr=60000.0 / nn, nn=nn, sdnn=float(np.std(ibi, ddof=1)), rmssd=float(np.sqrt(np.mean(d * d))) if len(d) else None,
             pnn50=float(100.0 * np.mean(np.abs(d) > 50)) if len(d) else None, hrmin=float(np.percentile(inst, 5)), hrmax=float(np.percentile(inst, 95)),
             pres=1, sqh=quality, sqr=0, mf=0.0)
    S["hra"] = S["hrs"] = S["hrb"] = S["hr"]            # reference rate: all estimators agree
    if resp is not None and fs_resp:
        a, c = int(t0 * fs_resp), int(t1 * fs_resp)
        x = np.asarray(resp[a:c], float)
        if len(x) > 10 * fs_resp and np.isfinite(x).all():
            x = x - np.mean(x)
            n = 1 << int(math.ceil(math.log2(len(x) * 4)))
            sp = np.abs(np.fft.rfft(x * np.hanning(len(x)), n)) ** 2; f = np.fft.rfftfreq(n, 1.0 / fs_resp)
            m = (f >= 0.1) & (f <= 1.0)
            if m.any() and sp[m].max() > 0:
                S["rr"] = float(60.0 * f[m][np.argmax(sp[m])]); S["rrs"] = S["rr"]; S["sqr"] = quality
                # breath-to-breath regularity from zero crossings of the smoothed signal
                s = np.convolve(x, np.ones(max(1, int(fs_resp * 0.5))) / max(1, int(fs_resp * 0.5)), "same")
                up = np.where((s[:-1] < 0) & (s[1:] >= 0))[0] / fs_resp
                if len(up) >= 4:
                    per = np.diff(up); S["pcv"] = float(np.std(per) / np.mean(per))
                    # pauses: no upward crossing for > 10 s
                    gaps = np.diff(np.concatenate([[0], up, [len(x) / fs_resp]]))
                    S["apl"] = float(gaps.max()) if gaps.max() > 6 else 0.0
                    S["ap5"] = int(np.sum(gaps > 10))
    return S


def label_window(S, age_years, annot=None):
    """annot: None | 'AF' | 'apnea' | 'cough' (from the dataset). Returns class index."""
    P = age_profile(age_years)
    if annot == "AF": return CLS["irregular rhythm"]
    if annot == "cough": return CLS["coughing"]
    if annot == "apnea": return CLS["apnoea or slow breathing"]
    if _fin(S.get("apl")) and S["apl"] > P["apnea"]: return CLS["apnoea or slow breathing"]
    if _fin(S.get("rmssd")) and _fin(S.get("nn")) and S["nn"] > 0 and S["rmssd"] / S["nn"] > 0.22 and _fin(S.get("pnn50")) and S["pnn50"] > 45:
        return CLS["irregular rhythm"]
    hr, rr = S.get("hr"), S.get("rr")
    if _fin(hr) and hr > P["hr"][1] * 1.05: return CLS["tachycardia"]
    if _fin(hr) and hr < P["hr"][0] * 0.95: return CLS["bradycardia"]
    if _fin(rr) and rr > P["rr"][1] * 1.1: return CLS["fast / laboured breathing"]
    if _fin(rr) and rr < P["rr"][0] * 0.8: return CLS["apnoea or slow breathing"]
    return CLS["normal"]


def windows(beat_t, total_s, win=60.0, step=30.0):
    t = 0.0
    while t + win <= total_s:
        yield t, t + win
        t += step


def augment_age(rng, S_age_years_choices=(30, 45, 70)):
    return float(rng.choice(S_age_years_choices))


def build_from_records(items, age_years=40.0, win=60.0, step=30.0):
    """items: iterable of (group_id, beat_times, resp, fs_resp, total_s, annot_fn) -> X, y, groups, rows.
    annot_fn(t0,t1) -> None|'AF'|'apnea'|'cough'."""
    X, y, g, rows = [], [], [], []
    for gid, bt, resp, fsr, tot, an in items:
        for t0, t1 in windows(bt, tot, win, step):
            S = status_from_beats(bt, resp, fsr, t0, t1)
            if S is None: continue
            lab = label_window(S, age_years, an(t0, t1) if an else None)
            X.append(features(S, age_profile(age_years))); y.append(lab); g.append(gid); rows.append(S)
    return np.array(X, np.float32), np.array(y, np.int64), np.array(g), rows


def load_user_bank(path, age_col="age", label_col="label"):
    """CSV with the monitor's status columns (hr, rr, nn, rmssd, sdnn, pnn50, hrmin, hrmax, sp, pcv, ampcv, ap5, apl, amp1, amp,
    ie, rrs, cgh, vhf, mf, sqh, sqr, pres ...) + a label column (class index or class name) + optional age (years)."""
    import pandas as pd
    df = pd.read_csv(path)
    X, y, rows = [], [], []
    for _, r in df.iterrows():
        S = {k: (None if (isinstance(v, float) and math.isnan(v)) else (float(v) if isinstance(v, (int, float, np.number)) else v)) for k, v in r.items()}
        age = S.get(age_col) if _fin(S.get(age_col)) else 40.0
        lab = S[label_col]
        lab = CLS[lab] if isinstance(lab, str) and lab in CLS else int(lab)
        X.append(features(S, age_profile(age))); y.append(lab); rows.append(S)
    return np.array(X, np.float32), np.array(y, np.int64), rows


def load_device_csv(path, label, age_years=40.0, win=60, step=30):
    """CSV exported from the monitor (Files page). One class label for the whole recording.
    Windows of `win` seconds -> status dicts (medians of the 1 Hz rows)."""
    import pandas as pd
    df = pd.read_csv(path, comment="#")
    X, y = [], []
    for a in range(0, max(1, len(df) - win + 1), step):
        w = df.iloc[a:a + win]
        hr = w["hr_bpm"].replace(0, np.nan); rr = w["rr_per_min"].replace(0, np.nan)
        if hr.notna().sum() < 5 and rr.notna().sum() < 5: continue
        S = dict(hr=float(hr.median()) if hr.notna().any() else None, rr=float(rr.median()) if rr.notna().any() else None,
                 pres=1 if hr.notna().sum() >= 5 else 0, sqh=float(w["signal_heart"].median()), sqr=float(w["signal_resp"].median()),
                 rmssd=float(w["rmssd_ms"].median()) if "rmssd_ms" in w and w["rmssd_ms"].notna().any() else None,
                 mf=float(w["motion"].mean()) if "motion" in w else 0.0)
        if S["hr"]: S["nn"] = 60000.0 / S["hr"]; S["hra"] = S["hrs"] = S["hrb"] = S["hr"]
        X.append(features(S, age_profile(age_years))); y.append(CLS[label] if isinstance(label, str) else int(label))
    return np.array(X, np.float32), np.array(y, np.int64)


# ---------------------------------------------------------------------------------------------
#  PhysioNet record loaders (need `pip install wfdb`; NOT exercised offline - verify on first Colab run)
# ---------------------------------------------------------------------------------------------
def _pick(names, *keys):
    for i, n in enumerate(names):
        if any(k in n.lower() for k in keys): return i
    return None


def _beats_from_ecg(sig, fs):
    import wfdb.processing as wp
    x = np.nan_to_num(np.asarray(sig, float))
    return np.asarray(wp.xqrs_detect(sig=x, fs=fs, verbose=False), float) / fs


def load_wfdb_items(key, root, age_years=40.0, max_records=None):
    """Returns items for build_from_records(). One group id per record -> subject-wise splitting."""
    import wfdb
    d = os.path.join(root, "datasets", key)
    recs = sorted({os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(d, "*.hea"))})
    if max_records: recs = recs[:max_records]
    items = []
    for r in recs:
        try:
            rec = wfdb.rdrecord(os.path.join(d, r)); names = rec.sig_name; fs = rec.fs; sig = rec.p_signal; tot = rec.sig_len / fs
            ie = _pick(names, "ecg")                                  # first ECG-like channel
            if ie is None: ie = next((i for i, n in enumerate(names) if n.strip().lower() in ("ii", "i", "mlii")), None)
            ir = _pick(names, "resp", "breath", "rsp")
            if ie is None: continue
            bt = _beats_from_ecg(sig[:, ie], fs)
            resp = sig[:, ir] if ir is not None else None
            an = None
            if key == "afdb":
                a = wfdb.rdann(os.path.join(d, r), "atr"); marks = [(s / fs, n) for s, n in zip(a.sample, a.aux_note)]
                def an(t0, t1, marks=marks):
                    cur = None; af = 0.0; last_t, last = 0.0, ""
                    for t, n in marks + [(tot, "")]:
                        if last == "(AFIB": af += max(0.0, min(t, t1) - max(last_t, t0))
                        if n: last = n
                        last_t = t
                    return "AF" if af > 0.5 * (t1 - t0) else None
            elif key == "apnea":
                a = wfdb.rdann(os.path.join(d, r), "apn"); sym = {int(s // (fs * 60)): c for s, c in zip(a.sample, a.symbol)}
                def an(t0, t1, sym=sym):
                    m = [sym.get(int(t // 60) + k, "N") for k in range(int((t1 - t0) // 60))]
                    return "apnea" if m and sum(c == "A" for c in m) * 2 >= len(m) else None
            items.append((f"{key}:{r}", bt, resp, fs if resp is not None else None, tot, an))
        except Exception as e:                                       # keep going: one bad record must not stop the run
            print(f"[skip] {key}/{r}: {e}")
    print(f"[{key}] {len(items)} usable records")
    return items
