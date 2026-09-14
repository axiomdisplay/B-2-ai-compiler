// bench: int_loop — djb2-style integer hash loop (Smi/ToInt32 hot path).
// JS twin of int_loop.tsbc (identical operation order — Rule 96 differential pair).
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now()
  : () => Date.now();

function kernel() {
  let h = 5381;
  for (let i = 0; i < 5000000; i++) { h = ((h << 5) + h + i) | 0; }
  return h >>> 0;
}

const t0 = now();
const r = kernel();
const t1 = now();
console.log("RESULT", r);
console.log("TIME", (t1 - t0).toFixed(2));
