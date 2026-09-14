// bench: array_loop — packed-Smi element store, then repeated full passes of
// element loads (JS twin of array_loop.tsbc, Rule 96 differential pair).
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now()
  : () => Date.now();

function kernel() {
  const A = [];
  for (let i = 0; i < 300; i++) A[i] = i * 2;
  let acc = 0;
  for (let p = 0; p < 3000; p++) {
    for (let i = 0; i < 300; i++) acc = (acc + A[i]) | 0;
  }
  return acc;
}

const t0 = now();
const r = kernel();
const t1 = now();
console.log("RESULT", r);
console.log("TIME", (t1 - t0).toFixed(2));
