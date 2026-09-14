// bench: float_loop — mandelbrot-lite iteration counts (JS twin of
// float_loop.tsbc; identical operation order, Rule 96 differential pair).
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now()
  : () => Date.now();

function kernel() {
  let acc = 0;
  for (let py = 0; py < 200; py++) {
    const y0 = py * 0.005 - 1.5;
    for (let px = 0; px < 200; px++) {
      const x0 = px * 0.005 - 0.5;
      let x = 0, y = 0, it = 0;
      while (x * x + y * y <= 4 && it < 100) {
        const xt = x * x - y * y + x0;
        y = 2 * x * y + y0;
        x = xt;
        it++;
      }
      acc += it;
    }
  }
  return acc;
}

const t0 = now();
const r = kernel();
const t1 = now();
console.log("RESULT", r);
console.log("TIME", (t1 - t0).toFixed(2));
