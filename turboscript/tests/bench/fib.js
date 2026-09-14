// bench: fib — naive double recursion (JS twin of fib.tsbc, Rule 96 pair).
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now()
  : () => Date.now();

function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

function kernel() { return fib(27); }

const t0 = now();
const r = kernel();
const t1 = now();
console.log("RESULT", r);
console.log("TIME", (t1 - t0).toFixed(2));
