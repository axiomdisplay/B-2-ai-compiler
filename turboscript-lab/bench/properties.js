// property access + shape transitions + method calls through ICs
function Vec(x, y, z) {
  this.x = x; this.y = y; this.z = z;
}
Vec.prototype.dot = function (o) {
  return this.x * o.x + this.y * o.y + this.z * o.z;
};
let acc = 0;
let a = new Vec(1, 2, 3), b = new Vec(4, 5, 6);
for (let i = 0; i < 2000000; i++) {
  a.x = i & 0xff;
  b.y = (i >> 3) & 0xff;
  acc = (acc + a.dot(b)) & 0xffffff;
}
print("checksum: " + acc);
