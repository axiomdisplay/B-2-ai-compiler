// bench: object_fields — object allocation + shape-transition stores and
// monomorphic IC loads (JS twin of object_fields.tsbc, Rule 96 pair).
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now()
  : () => Date.now();

function kernel() {
  let acc = 0;
  for (let i = 0; i < 1000000; i++) {
    const o = {};
    o.a = i;
    o.b = i * 2;
    o.c = i * 3;
    acc = (acc + o.a + o.b + o.c) | 0;
  }
  return acc;
}

const t0 = now();
const r = kernel();
const t1 = now();
console.log("RESULT", r);
console.log("TIME", (t1 - t0).toFixed(2));
