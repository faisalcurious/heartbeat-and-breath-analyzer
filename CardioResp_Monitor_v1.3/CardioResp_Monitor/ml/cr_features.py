"""
cr_features.py - feature definition shared by the firmware (Analytics.h), the
dashboard (web/analytics.js) and this training code.  The three ports MUST
produce identical numbers: tests/ cross-check them.

Input  : a "status" dictionary with the same keys the device streams
         (hr, rr, rmssd, ...)  and the age profile (hr range, rr range, apnea s)
Output : N_IN = 21 floats, each roughly in 0..2 (or -2..2), so no further
         standardisation is needed on the device.
"""
import math

FEATURE_NAMES = [
    "hr_z", "rr_z", "hr_avail", "rr_avail",
    "rmssd_rel", "sdnn_rel", "pnn50", "hr_spread", "hr_disagree",
    "card_spec_prom", "breath_period_cv", "breath_amp_cv", "pause_count", "pause_len",
    "depth_ratio", "ie_ratio", "rr_disagree", "cough_rate", "vib_hf_share",
    "motion", "signal_quality",
]
N_IN = len(FEATURE_NAMES)
CLASS_NAMES = ["normal", "tachycardia", "bradycardia", "irregular rhythm",
               "fast / laboured breathing", "apnoea or slow breathing", "coughing"]
N_CLASS = len(CLASS_NAMES)


def _num(v):
    return v is not None and isinstance(v, (int, float)) and math.isfinite(v)


def _clip(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def features(S, P):
    """S: status dict, P: dict with hr=(lo,hi), rr=(lo,hi), apnea (seconds)."""
    x = [0.0] * N_IN
    hr, rr = S.get("hr"), S.get("rr")
    hrOk = bool(S.get("pres", 1)) and _num(hr)
    rrOk = _num(rr)
    hmid, hhalf = 0.5 * (P["hr"][0] + P["hr"][1]), 0.5 * (P["hr"][1] - P["hr"][0])
    rmid, rhalf = 0.5 * (P["rr"][0] + P["rr"][1]), 0.5 * (P["rr"][1] - P["rr"][0])
    if hrOk:
        x[0] = _clip((hr - hmid) / hhalf, -4.0, 4.0) / 2.0
        x[2] = 1.0
    if rrOk:
        x[1] = _clip((rr - rmid) / rhalf, -4.0, 4.0) / 2.0
        x[3] = 1.0
    nn = S.get("nn")
    if hrOk and _num(nn) and nn > 200:
        if _num(S.get("rmssd")): x[4] = _clip(S["rmssd"] / nn, 0.0, 0.4) * 5.0
        if _num(S.get("sdnn")): x[5] = _clip(S["sdnn"] / nn, 0.0, 0.4) * 5.0
    if hrOk and _num(S.get("pnn50")): x[6] = _clip(S["pnn50"], 0.0, 100.0) / 50.0
    if hrOk and _num(S.get("hrmin")) and _num(S.get("hrmax")) and hr > 0:
        x[7] = _clip((S["hrmax"] - S["hrmin"]) / hr, 0.0, 1.0) * 3.0
    if hrOk and hr > 0:
        d = 0.0
        for k in ("hra", "hrs", "hrb"):
            if _num(S.get(k)): d = max(d, abs(S[k] - hr))
        x[8] = _clip(d / hr, 0.0, 0.5) * 4.0
    if hrOk and _num(S.get("sp")): x[9] = _clip(S["sp"], 0.0, 12.0) / 6.0
    if rrOk and _num(S.get("pcv")): x[10] = _clip(S["pcv"], 0.0, 1.0) * 3.0
    if rrOk and _num(S.get("ampcv")): x[11] = _clip(S["ampcv"], 0.0, 1.5) * 2.0
    if _num(S.get("ap5")): x[12] = _clip(S["ap5"], 0.0, 6.0) / 3.0
    if _num(S.get("apl")) and P["apnea"] > 0:
        cur = S["apc"] if _num(S.get("apc")) else 0.0
        x[13] = _clip(max(S["apl"], cur) / P["apnea"], 0.0, 2.0)
    x[14] = 1.0
    if rrOk and _num(S.get("amp1")) and _num(S.get("amp")) and S["amp"] > 1e-6:
        x[14] = _clip(S["amp1"] / S["amp"], 0.0, 2.0)
    x[15] = 0.8
    if rrOk and _num(S.get("ie")): x[15] = _clip(S["ie"], 0.0, 2.0)
    if rrOk and rr > 0 and _num(S.get("rrs")): x[16] = _clip(abs(S["rrs"] - rr) / rr, 0.0, 0.6) * 3.0
    if _num(S.get("cgh")): x[17] = _clip(S["cgh"], 0.0, 10.0) / 5.0
    if _num(S.get("vhf")): x[18] = _clip(S["vhf"], 0.0, 1.0) * 2.0
    if _num(S.get("mf")): x[19] = _clip(S["mf"], 0.0, 1.0) * 2.0
    sh = S.get("sqh", 0) if _num(S.get("sqh")) else 0
    sr = S.get("sqr", 0) if _num(S.get("sqr")) else 0
    x[20] = (sh + sr) / 200.0
    return x


# ---- age table (mirror of AgeNorms.h / core.js) ----------------------------------
AGE_TABLE = [
    dict(id=0, name="Newborn (0-1 month)", hr=(100, 205), rr=(30, 60), apnea=20),
    dict(id=1, name="Infant (1-12 months)", hr=(100, 180), rr=(30, 53), apnea=20),
    dict(id=2, name="Toddler (1-2 years)", hr=(98, 140), rr=(22, 37), apnea=15),
    dict(id=3, name="Preschool (3-5 years)", hr=(80, 120), rr=(20, 28), apnea=15),
    dict(id=4, name="School age (6-11 years)", hr=(75, 118), rr=(18, 25), apnea=15),
    dict(id=5, name="Adolescent (12-17 years)", hr=(60, 100), rr=(12, 20), apnea=10),
    dict(id=6, name="Adult (18-39 years)", hr=(60, 100), rr=(12, 20), apnea=10),
    dict(id=7, name="Adult (40-64 years)", hr=(60, 100), rr=(12, 20), apnea=10),
    dict(id=8, name="Older adult (65+ years)", hr=(60, 100), rr=(12, 20), apnea=10),
]


def age_profile(years):
    lim = [1 / 12, 1, 3, 6, 12, 18, 40, 65]
    i = 0
    while i < len(lim) and years >= lim[i]:
        i += 1
    return AGE_TABLE[i]


# ---- model ------------------------------------------------------------------------
def forward(x, W1, b1, W2, b2):
    """x: list[N_IN]; W1: [H][N_IN]; W2: [N_CLASS][H]. Returns softmax list."""
    h = [max(0.0, sum(w * v for w, v in zip(row, x)) + b) for row, b in zip(W1, b1)]
    z = [sum(w * v for w, v in zip(row, h)) + b for row, b in zip(W2, b2)]
    m = max(z)
    e = [math.exp(v - m) for v in z]
    s = sum(e)
    return [v / s for v in e]
