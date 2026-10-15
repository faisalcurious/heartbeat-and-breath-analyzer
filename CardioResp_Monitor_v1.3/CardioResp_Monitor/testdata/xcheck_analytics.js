// helper for xcheck_analytics.py : evaluates the JS analytics on the same case file, prints JSON array
const fs = require("fs"), path = require("path");
const core = require("../web/core.js");
const A = require("../web/analytics.js");
const M = require("../web/mlmodel.js");
const lines = fs.readFileSync(process.argv[2], "utf8").split("\n").filter(Boolean);
const T = new A.AnaTracker();
const out = [];
for (const ln of lines) {
  const S = {}; let age = 30, t = 0, reset = false;
  for (const tok of ln.split(" ")) {
    const [k, v] = tok.split("=");
    if (k === "age") age = parseFloat(v); else if (k === "t") t = parseInt(v); else if (k === "reset") reset = v === "1";
    else S[k] = v === "nan" ? NaN : parseFloat(v);
  }
  if (reset) T.reset();
  const s = A.anaInputs(S);
  const P = core.ageProfile(age);
  T.update(s, P, t);
  const x = A.anaFeatures(s, P);
  const v = A.anaEvaluate(T, s, P, t, M);
  out.push({x, v});
}
console.log(JSON.stringify(out));
