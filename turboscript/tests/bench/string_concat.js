// bench: string_concat — string allocation + concatenation throughput
// (JS twin of string_concat.tsbc, Rule 96 differential pair).
const now = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now()
  : () => Date.now();

function kernel() {
  let acc = 0;
  for (let i = 0; i < 20000; i++) {
    let s = "";
    for (let j = 0; j < 40; j++) { s += "x"; }
    let u = "id:";
    u += i;
    acc += s.length + u.length;
  }
  return acc;
}

const t0 = now();
const r = kernel();
const t1 = now();
console.log("RESULT", r);
console.log("TIME", (t1 - t0).toFixed(2));
