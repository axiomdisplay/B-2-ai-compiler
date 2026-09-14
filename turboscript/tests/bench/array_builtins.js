// bench: array_builtins — builtin-layer pipeline (JS twin of
// array_builtins.tsbc, Rule 96 pair). 100k push + 3 x 100k callback calls.
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now()
  : () => Date.now();

function kernel() {
  const a = [];
  let i = 0;
  while (i < 100000) { a.push(i % 7); i++; }
  const doubled = a.map(function (x) { return x * 2; });
  const sum = doubled.reduce(function (acc, v) { return acc + v; }, 0);
  const odds = doubled.filter(function (v) { return v % 2 === 1; });
  return sum + odds.length;
}

const t0 = now();
const r = kernel();
const t1 = now();
console.log("RESULT", r);
console.log("TIME", (t1 - t0).toFixed(2));
