"""
cr_train.py - train the tiny MLP, write figures + parameters + data to a results folder (Google Drive in Colab),
and export MlModel.h / mlmodel.js / model.json for the firmware and the dashboard.
"""
import os, sys, json, time, platform
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cr_features import N_IN, N_CLASS, CLASS_NAMES, FEATURE_NAMES, forward
import train_baseline as tb


def train(X, y, groups=None, hid=14, alpha=2e-3, seed=3, test_frac=0.2, sample_weight=None):
    from sklearn.neural_network import MLPClassifier
    from sklearn.model_selection import GroupShuffleSplit, train_test_split
    if groups is not None and len(set(groups)) > 4:
        tr, te = next(GroupShuffleSplit(n_splits=1, test_size=test_frac, random_state=seed).split(X, y, groups))   # subject-wise split
    else:
        tr, te = train_test_split(np.arange(len(y)), test_size=test_frac, random_state=seed, stratify=y)
    clf = MLPClassifier(hidden_layer_sizes=(hid,), activation="relu", alpha=alpha, max_iter=500, early_stopping=True,
                        n_iter_no_change=15, random_state=seed)
    clf.fit(X[tr], y[tr])
    return clf, tr, te


def evaluate(clf, X, y, te):
    from sklearn.metrics import confusion_matrix, classification_report, accuracy_score
    p = clf.predict(X[te]); pr = clf.predict_proba(X[te])
    labs = list(range(N_CLASS))
    return dict(te_idx=te, accuracy=float(accuracy_score(y[te], p)), confusion=confusion_matrix(y[te], p, labels=labs).tolist(),
                report=classification_report(y[te], p, labels=labs, target_names=CLASS_NAMES, output_dict=True, zero_division=0),
                proba=pr, pred=p)


def figures(clf, X, y, te, ev, outdir, tag):
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    os.makedirs(outdir, exist_ok=True); paths = []
    cm = np.array(ev["confusion"], float); cmn = cm / np.maximum(cm.sum(1, keepdims=True), 1)
    fig, ax = plt.subplots(figsize=(6.4, 5.6)); im = ax.imshow(cmn, cmap="Blues", vmin=0, vmax=1)
    ax.set_xticks(range(N_CLASS)); ax.set_yticks(range(N_CLASS)); ax.set_xticklabels(CLASS_NAMES, rotation=40, ha="right"); ax.set_yticklabels(CLASS_NAMES)
    for i in range(N_CLASS):
        for j in range(N_CLASS): ax.text(j, i, f"{cmn[i, j]:.2f}\n({int(cm[i, j])})", ha="center", va="center", fontsize=6, color="white" if cmn[i, j] > .5 else "black")
    ax.set_xlabel("predicted"); ax.set_ylabel("true"); ax.set_title(f"Confusion matrix (row-normalised), accuracy {ev['accuracy']:.3f}"); fig.colorbar(im); fig.tight_layout()
    paths.append(_save(fig, outdir, f"{tag}_confusion"))
    fig, ax = plt.subplots(figsize=(5.6, 4)); ax.plot(clf.loss_curve_, label="train loss")
    if getattr(clf, "validation_scores_", None): ax2 = ax.twinx(); ax2.plot(clf.validation_scores_, "r", label="validation accuracy"); ax2.set_ylabel("validation accuracy")
    ax.set_xlabel("epoch"); ax.set_ylabel("loss"); ax.set_title("Training curve"); fig.tight_layout(); paths.append(_save(fig, outdir, f"{tag}_training"))
    from sklearn.metrics import roc_curve, auc
    fig, ax = plt.subplots(figsize=(5.6, 4.4))
    for c in range(N_CLASS):
        yt = (y[te] == c).astype(int)
        if yt.sum() == 0 or yt.sum() == len(yt): continue
        fpr, tpr, _ = roc_curve(yt, ev["proba"][:, c]); ax.plot(fpr, tpr, label=f"{CLASS_NAMES[c]} (AUC {auc(fpr, tpr):.2f})")
    ax.plot([0, 1], [0, 1], "k:", lw=.8); ax.set_xlabel("false-positive rate"); ax.set_ylabel("true-positive rate"); ax.legend(fontsize=6); ax.set_title("One-vs-rest ROC"); fig.tight_layout()
    paths.append(_save(fig, outdir, f"{tag}_roc"))
    fig, ax = plt.subplots(figsize=(7, 3.6)); W = np.abs(clf.coefs_[0]).sum(1); o = np.argsort(W)[::-1]
    ax.bar(range(N_IN), W[o]); ax.set_xticks(range(N_IN)); ax.set_xticklabels([FEATURE_NAMES[i] for i in o], rotation=70, ha="right", fontsize=7)
    ax.set_title("Input importance (sum |first-layer weights|)"); fig.tight_layout(); paths.append(_save(fig, outdir, f"{tag}_feature_importance"))
    fig, ax = plt.subplots(figsize=(5.6, 3.6)); ax.bar(range(N_CLASS), np.bincount(y, minlength=N_CLASS)); ax.set_xticks(range(N_CLASS)); ax.set_xticklabels(CLASS_NAMES, rotation=40, ha="right", fontsize=7)
    ax.set_title("Samples per class"); fig.tight_layout(); paths.append(_save(fig, outdir, f"{tag}_class_balance"))
    return paths


def _save(fig, outdir, name):
    import matplotlib.pyplot as plt
    p = os.path.join(outdir, name)
    fig.savefig(p + ".png", dpi=300); fig.savefig(p + ".pdf"); plt.close(fig)
    return p + ".png"


def save_run(out_root, X, y, groups, clf, ev, info, tag=None, hid=14):
    """Writes everything needed to publish the model: data, parameters, metrics, figures."""
    tag = tag or time.strftime("run_%Y%m%d_%H%M%S")
    d = os.path.join(out_root, "results", tag); os.makedirs(d, exist_ok=True)
    np.savez_compressed(os.path.join(d, "training_data.npz"), X=X, y=y, groups=np.asarray(groups) if groups is not None else np.zeros(len(y), int),
                        feature_names=np.array(FEATURE_NAMES), class_names=np.array(CLASS_NAMES))
    import csv
    with open(os.path.join(d, "training_data.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(FEATURE_NAMES + ["label"]); [w.writerow(list(map(float, a)) + [int(b)]) for a, b in zip(X, y)]
    W1 = clf.coefs_[0].T; b1 = clf.intercepts_[0]; W2 = clf.coefs_[1].T; b2 = clf.intercepts_[1]
    meta = dict(info, tag=tag, hidden=hid, n_samples=int(len(y)), accuracy=ev["accuracy"], confusion=ev["confusion"], report=ev["report"],
                params=int(W1.size + b1.size + W2.size + b2.size), sklearn_params=clf.get_params(), python=platform.python_version(),
                numpy=np.__version__, class_names=CLASS_NAMES, feature_names=FEATURE_NAMES)
    json.dump(meta, open(os.path.join(d, "metrics_and_parameters.json"), "w"), indent=1, default=str)
    figs = figures(clf, X, y, ev["te_idx"], ev, os.path.join(d, "figures"), "fig")
    m = tb.export(W1, b1, W2, b2, dict(info, version=info.get("version", tag)), root=d_export(d))
    return d, figs, m


def d_export(d):
    """export() writes ml/model.json, web/mlmodel.js and CardioRespMonitor/MlModel.h under a root: build that tree in the run folder."""
    for sub in ("ml", "web", "CardioRespMonitor"): os.makedirs(os.path.join(d, "export", sub), exist_ok=True)
    return os.path.join(d, "export")


def check_export(model_json, X, n=200):
    """Python forward pass of the exported JSON equals sklearn (guards the C++/JS ports' weights)."""
    m = json.load(open(model_json))
    return float(np.mean([max(forward(list(map(float, x)), m["W1"], m["b1"], m["W2"], m["b2"])) for x in X[:n]]))
