"""
cr_synth.py - synthetic "status" generator for the baseline model.
Physiologically plausible value ranges per condition (reference ranges from
AgeNorms.h).  Only a BASELINE: retrain with real data in the Colab notebook.
"""
import numpy as np
from cr_features import AGE_TABLE, CLASS_NAMES, features, N_CLASS


def _u(r, a, b): return float(r.uniform(a, b))


def make_status(rng, cls, age_row=None):
    A = age_row or AGE_TABLE[int(rng.choice(len(AGE_TABLE), p=[.03, .05, .06, .07, .08, .08, .30, .22, .11]))]
    hlo, hhi = A["hr"]; rlo, rhi = A["rr"]; ap = A["apnea"]
    hw, rw = hhi - hlo, rhi - rlo
    # healthy baseline ------------------------------------------------------------
    hr = _u(rng, hlo + .05 * hw, hhi - .05 * hw)
    rr = _u(rng, rlo + .05 * rw, rhi - .05 * rw)
    S = dict(pres=1)
    rm_rel = _u(rng, .012, .09); sd_rel = rm_rel * _u(rng, 1.0, 2.0); pnn = _u(rng, 2, 40)
    spread = _u(rng, .05, .22); dis = abs(rng.normal(0, .02)); sp = _u(rng, 3, 10)
    pcv = _u(rng, .05, .22); acv = _u(rng, .1, .4); ap5 = 0; apl = 0; apc = 0
    depth = float(np.clip(rng.normal(1.0, .12), .5, 1.6)); ie = _u(rng, .5, 1.0)
    rdis = abs(rng.normal(0, .04)); cough = 0.0; vhf = _u(rng, 0, .22); mf = _u(rng, 0, .12)
    sqh = _u(rng, 45, 95); sqr = _u(rng, 45, 95)
    if cls == 1:      # tachycardia
        hr = hhi * _u(rng, 1.03, 1.9); rm_rel *= _u(rng, .4, 1.0); sd_rel *= _u(rng, .4, 1.0)
    elif cls == 2:    # bradycardia
        hr = hlo * _u(rng, .45, .97); rm_rel *= _u(rng, .8, 1.8)
    elif cls == 3:    # irregular rhythm
        hr = _u(rng, hlo * .7, hhi * 1.4)
        rm_rel = _u(rng, .11, .35); sd_rel = rm_rel * _u(rng, .9, 1.5); pnn = _u(rng, 40, 92)
        spread = _u(rng, .25, .65); dis = _u(rng, .06, .3); sp = _u(rng, .6, 3.2)
    elif cls == 4:    # fast / laboured breathing
        rr = rhi * _u(rng, 1.08, 2.1); depth = _u(rng, .35, .95); acv = _u(rng, .25, .8)
        ie = _u(rng, .85, 1.5); pcv = _u(rng, .12, .4)
        if rng.random() < .5: hr = hhi * _u(rng, .95, 1.4)
    elif cls == 5:    # apnoea / slow breathing
        if rng.random() < .6:
            ap5 = float(rng.integers(1, 6)); apl = ap * _u(rng, 1.0, 2.6); apc = apl if rng.random() < .3 else 0
            pcv = _u(rng, .3, .8); acv = _u(rng, .4, 1.2)
        else:
            rr = rlo * _u(rng, .35, .95); pcv = _u(rng, .15, .6); depth = _u(rng, .6, 1.3)
    elif cls == 6:    # coughing
        cough = _u(rng, 2, 10); vhf = _u(rng, .3, .95); mf = _u(rng, .05, .45)
        pcv = _u(rng, .15, .5); acv = _u(rng, .3, 1.0); sqh *= _u(rng, .6, 1.0)
        if rng.random() < .5: rr = rhi * _u(rng, 1.0, 1.5)
    # hard cases: noisy/unavailable inputs -----------------------------------------
    if rng.random() < .10:
        sqh = _u(rng, 10, 40); sqr = _u(rng, 10, 40)
        dis += _u(rng, 0, .08); sp *= _u(rng, .5, 1.0); mf = min(1, mf + _u(rng, 0, .3))
    nn = 60000.0 / hr
    S.update(hr=hr, rr=rr, nn=nn, rmssd=rm_rel * nn, sdnn=sd_rel * nn, pnn50=pnn,
             hrmin=hr * (1 - spread / 2), hrmax=hr * (1 + spread / 2),
             hra=hr * (1 + (dis if rng.random() < .5 else -dis)), hrs=hr * (1 + rng.normal(0, .01 + dis / 2)),
             hrb=hr * (1 + rng.normal(0, .01)), sp=sp, pcv=pcv, ampcv=acv, ap5=ap5, apl=apl, apc=apc,
             amp1=depth, amp=1.0, ie=ie, rrs=rr * (1 + (rdis if rng.random() < .5 else -rdis)),
             cgh=cough, vhf=vhf, mf=mf, sqh=sqh, sqr=sqr)
    # unavailable heart/breath (about 8 % of samples): keep only the other channel
    if cls not in (1, 2, 3) and rng.random() < .08:
        S["pres"] = 0
    if cls in (0, 1, 2, 3, 6) and rng.random() < .05:
        S["rr"] = None
    return S, A


def dataset(n, seed=1, balance=None):
    rng = np.random.default_rng(seed)
    p = balance or [.28, .13, .12, .14, .12, .11, .10]
    X, y = [], []
    for _ in range(n):
        c = int(rng.choice(N_CLASS, p=p))
        S, A = make_status(rng, c)
        # label follows what is observable: tachy/brady/irregular need a heart signal
        if not S["pres"] and c in (1, 2, 3):
            c = 0
        if S["rr"] is None and c in (4, 5):
            c = 0
        X.append(features(S, A)); y.append(c)
    return np.array(X, dtype=np.float32), np.array(y, dtype=np.int64)
