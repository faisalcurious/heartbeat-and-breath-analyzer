"""Writes ml/CardioResp_Colab.ipynb (no nbformat needed)."""
import json, os
cells = []
def md(s): cells.append({"cell_type": "markdown", "metadata": {}, "source": s.strip("\n").splitlines(True)})
def code(s): cells.append({"cell_type": "code", "metadata": {}, "execution_count": None, "outputs": [], "source": s.strip("\n").splitlines(True)})

md("""
# CardioResp Monitor - model training (Colab)

Trains the tiny model (21 inputs -> 14 hidden ReLU -> 7 classes) that runs **on the controller** (`MlModel.h`) and **in the web page** (`mlmodel.js`).

* Datasets are downloaded **once** into your Google Drive (`MyDrive/CardioResp/datasets/...`); later runs reuse them.
* Everything needed for a publication is saved to `MyDrive/CardioResp/results/<run>/`: training data (`.npz` + `.csv`), parameters and metrics (`.json`), figures (300 dpi `.png` + vector `.pdf`) and the exported firmware / web model files.
* You can add **your own data banks** (section 5).

**Read this before trusting any number.** Public ECG/respiration databases give heart/breathing *rate and variability*, not the monitor's chest-vibration quality indices. Fields a dataset cannot provide stay empty and the model uses neutral defaults. Cough has no public labelled source here (keep the synthetic samples or add your own labelled data). Labels from annotations/age limits are *weak labels*. Report accuracy only from the subject-wise test split, and validate on your own monitor recordings before any clinical claim. The model is an early-warning aid, not a diagnostic device.
""")
md("## 1. Setup: Google Drive and project code")
code("""
import os, sys, subprocess, json, zipfile, glob
IN_COLAB = 'google.colab' in sys.modules
if IN_COLAB:
    from google.colab import drive
    drive.mount('/content/drive')
    ROOT = '/content/drive/MyDrive/CardioResp'
else:
    ROOT = os.environ.get('CR_ROOT', os.path.abspath('CardioResp_drive'))
os.makedirs(ROOT, exist_ok=True)
print('Storage folder:', ROOT)
subprocess.run([sys.executable, '-m', 'pip', 'install', '-q', 'wfdb', 'scikit-learn', 'matplotlib', 'pandas'], check=False)
""")
code("""
# The code lives in the 'ml' folder of the project. Put that folder in Drive (MyDrive/CardioResp/ml) once,
# or upload cardioresp_ml.zip when asked.
ML = os.path.join(ROOT, 'ml')
if not os.path.exists(os.path.join(ML, 'cr_features.py')):
    here = os.path.dirname(os.path.abspath('__file__'))
    if os.path.exists(os.path.join(here, 'cr_features.py')):
        ML = here
    elif IN_COLAB:
        from google.colab import files
        print('Upload cardioresp_ml.zip (the ml folder zipped)')
        up = files.upload(); z = list(up)[0]
        os.makedirs(ML, exist_ok=True); zipfile.ZipFile(z).extractall(ML)
        sub = glob.glob(os.path.join(ML, '*', 'cr_features.py'))
        if sub: ML = os.path.dirname(sub[0])
sys.path.insert(0, ML)
import numpy as np
import cr_features, cr_real as R, cr_train as T
from cr_features import CLASS_NAMES, FEATURE_NAMES
print('code folder:', ML, '| classes:', CLASS_NAMES)
""")
md("""
## 2. Choose the data sources
Set a flag to `False` to skip a source. `MAX_RECORDS` limits the records per database (use `None` for all). Downloads are cached: the second run prints `[cache]`.
""")
code("""
USE = {'cebs': True, 'afdb': True, 'apnea': True, 'bidmc': True}
MAX_RECORDS = 20          # None = everything (large downloads)
AGE_YEARS = 40.0          # age used for the age-referenced limits when a database does not provide one
N_SYNTH = 30000           # synthetic samples to add (needed for cough and for balance); 0 = none
USER_BANK_CSV = None      # e.g. ROOT + '/mybank.csv'  (status columns + 'label' [+ 'age'])
DEVICE_CSVS = {}          # e.g. {ROOT + '/mydata/rest.csv': 'normal', ROOT + '/mydata/cough.csv': 'coughing'}
""")
md("## 3. Download datasets once into Drive")
code("""
paths = {}
for k, on in USE.items():
    if not on: continue
    try:
        paths[k] = R.ensure_physionet(k, ROOT)
    except Exception as e:
        print(f'[warn] {k} not available: {e}')
        USE[k] = False
print(paths)
""")
md("## 4. Turn the records into model inputs (60 s windows, 30 s step)")
code("""
Xs, ys, gs = [], [], []
for k, on in USE.items():
    if not on: continue
    items = R.load_wfdb_items(k, ROOT, age_years=AGE_YEARS, max_records=MAX_RECORDS)
    if not items: continue
    X, y, g, rows = R.build_from_records(items, age_years=AGE_YEARS)
    print(k, X.shape, dict(zip(CLASS_NAMES, np.bincount(y, minlength=len(CLASS_NAMES)).tolist())))
    Xs.append(X); ys.append(y); gs.append(g)
""")
md("""
## 5. Your own data banks (optional)
* `USER_BANK_CSV`: a table with the monitor's status columns (`hr, rr, nn, rmssd, sdnn, pnn50, hrmin, hrmax, sp, pcv, ampcv, ap5, apl, amp1, amp, ie, rrs, cgh, vhf, mf, sqh, sqr, pres`), a `label` column (class name or index) and optionally `age` in years.
* `DEVICE_CSVS`: CSV files downloaded from the monitor's **Files** page; give one class label per recording (for example record someone coughing on purpose and label it `coughing`).
""")
code("""
if USER_BANK_CSV and os.path.exists(USER_BANK_CSV):
    X, y, rows = R.load_user_bank(USER_BANK_CSV)
    Xs.append(X); ys.append(y); gs.append(np.array(['bank%d' % (i // 50) for i in range(len(y))])); print('user bank', X.shape)
for p, lab in DEVICE_CSVS.items():
    X, y = R.load_device_csv(p, lab, AGE_YEARS)
    if len(y): Xs.append(X); ys.append(y); gs.append(np.array([os.path.basename(p)] * len(y))); print(p, X.shape)
if N_SYNTH:
    from cr_synth import dataset
    Xa, ya = dataset(N_SYNTH, seed=11)
    Xs.append(Xa); ys.append(ya); gs.append(np.array(['syn%d' % (i % 40) for i in range(len(ya))])); print('synthetic', Xa.shape)
X = np.vstack(Xs); y = np.concatenate(ys); groups = np.concatenate(gs)
print('total', X.shape, dict(zip(CLASS_NAMES, np.bincount(y, minlength=len(CLASS_NAMES)).tolist())))
""")
md("## 6. Train (subject-wise test split), evaluate")
code("""
HID = 14                  # must stay 14 unless you also change the firmware constants
clf, tr, te = T.train(X, y, groups, hid=HID)
ev = T.evaluate(clf, X, y, te)
print('test accuracy (subject-wise):', round(ev['accuracy'], 4))
if not any(USE.values()) and not USER_BANK_CSV and not DEVICE_CSVS:
    print('WARNING: only synthetic data was used. This accuracy only says the model learned the synthetic generator; it is NOT evidence of clinical performance.')
import pandas as pd
print(pd.DataFrame(ev['report']).T.round(3).to_string())
""")
md("## 7. Save data, parameters, figures and the exported model to Drive")
code("""
info = dict(version='colab-' + __import__('time').strftime('%Y%m%d'), trained_on=', '.join(k for k, v in USE.items() if v) + (' + own bank' if USER_BANK_CSV else '') + (' + synthetic' if N_SYNTH else ''),
            n_train=int(len(tr)), n_test=int(len(te)), test_accuracy=round(ev['accuracy'], 4), age_years_default=AGE_YEARS, max_records=MAX_RECORDS)
run_dir, figs, model = T.save_run(ROOT, X, y, groups, clf, ev, info, hid=HID)
print('saved to', run_dir)
for f in figs: print(' ', f)
try:
    from IPython.display import Image, display
    for f in figs[:3]: display(Image(f))
except ImportError:
    pass
print('mean max-probability on the first 200 samples (python forward pass of the exported json):', round(T.check_export(os.path.join(run_dir, 'export/ml/model.json'), X), 3))
""")
md("""
## 8. Put the model into the firmware and the web page
Copy the three files from `results/<run>/export/` into the project (same relative folders), rebuild the page with `python3 tools_build_page.py`, compile and flash:

* `export/CardioRespMonitor/MlModel.h` -> `CardioRespMonitor/MlModel.h`
* `export/web/mlmodel.js` -> `web/mlmodel.js`
* `export/ml/model.json` -> `ml/model.json`

Then run `host_test/analytics_test` and `testdata/xcheck_analytics.*` again: they check that C++, JavaScript and Python give the same verdicts for the new weights.
""")
code("""
zpath = os.path.join(run_dir, 'cardioresp_model_export.zip')
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
    for f in glob.glob(os.path.join(run_dir, 'export', '**', '*.*'), recursive=True): z.write(f, os.path.relpath(f, os.path.join(run_dir, 'export')))
print(zpath)
if IN_COLAB:
    from google.colab import files; files.download(zpath)
""")
nb = {"cells": cells, "metadata": {"kernelspec": {"display_name": "Python 3", "language": "python", "name": "python3"}, "language_info": {"name": "python"}, "colab": {"provenance": []}}, "nbformat": 4, "nbformat_minor": 5}
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "CardioResp_Colab.ipynb")
json.dump(nb, open(out, "w"), indent=1); print(out, len(cells), "cells")
