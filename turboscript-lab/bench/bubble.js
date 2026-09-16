// bubble sort: array element access, int math, loops
let N = 800;
let arr = new Array(N);
let seed = 12345;
for (let i = 0; i < N; i++) {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  arr[i] = seed;
}
let total = 0;
for (let round = 0; round < 12; round++) {
  let a = arr.slice(0);
  for (let i = 0; i < N - 1; i++) {
    for (let j = 0; j < N - 1 - i; j++) {
      if (a[j] > a[j + 1]) {
        let t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
      }
    }
  }
  total += a[0] + a[N - 1];
}
print("checksum: " + total);
