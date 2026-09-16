// n-body: float math + object property access (Java port of the classic bench)
function advance(bodies, dt, n) {
  for (let i = 0; i < n; i++) {
    let b = bodies[i];
    let vx = b.vx, vy = b.vy, vz = b.vz;
    for (let j = i + 1; j < n; j++) {
      let b2 = bodies[j];
      let dx = b.x - b2.x, dy = b.y - b2.y, dz = b.z - b2.z;
      let d2 = dx * dx + dy * dy + dz * dz;
      let mag = dt / (d2 * Math.sqrt(d2));
      let m2 = b2.mass * mag;
      vx -= dx * m2; vy -= dy * m2; vz -= dz * m2;
      let m1 = b.mass * mag;
      b2.vx += dx * m1; b2.vy += dy * m1; b2.vz += dz * m1;
    }
    b.vx = vx; b.vy = vy; b.vz = vz;
    b.x += dt * vx; b.y += dt * vy; b.z += dt * vz;
  }
}
function energy(bodies, n) {
  let e = 0;
  for (let i = 0; i < n; i++) {
    let b = bodies[i];
    e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
    for (let j = i + 1; j < n; j++) {
      let b2 = bodies[j];
      let dx = b.x - b2.x, dy = b.y - b2.y, dz = b.z - b2.z;
      e -= (b.mass * b2.mass) / Math.sqrt(dx * dx + dy * dy + dz * dz);
    }
  }
  return e;
}
let bodies = [
  {x:0, y:0, z:0, vx:0, vy:0, vz:0, mass:1},
  {x:4.84143144246472090, y:-1.16032004402742839, z:-0.103622044471123109,
   vx:0.606326392995830426, vy:2.79858685889772110, vz:-0.0252631936300385567, mass:0.000954791938424326609},
  {x:8.34336671824457987, y:4.12479856412430479, z:-0.403523417114321381,
   vx:-1.01077436603357559, vy:1.82581443234967149, vz:0.00841576453478634261, mass:0.000285885980666130812},
  {x:12.8943695621391310, y:-15.1111514016986312, z:-0.223307578892655734,
   vx:1.08279786382406622, vy:0.868768528059192691, vz:-0.0108326921383716498, mass:0.0000436624404335156298},
  {x:15.3796971148509165, y:-25.9193146099879641, z:0.179258772950371181,
   vx:0.978855074732062382, vy:-0.594654745767375546, vz:-0.0347553924770050832, mass:0.00000515138902046611451}
];
for (let w = 0; w < 50000; w++) advance(bodies, 0.01, 5);
let e = energy(bodies, 5);
print("checksum: " + e);
