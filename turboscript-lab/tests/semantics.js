// TurboScript Tier 0 semantics suite (Part 0 contract spot checks)
let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }

// numbers / NaN / -0  (Part 0 numeric semantics)
ok(NaN !== NaN, "NaN !== NaN");
ok(isNaN(NaN + 1), "NaN + 1 is NaN");
ok(isNaN(0 / 0), "0/0 is NaN");
ok(1 / 0 === Infinity, "1/0 = Infinity");
ok(-1 / 0 === -Infinity, "-1/0 = -Infinity");
let negzero = -0;
ok(negzero === 0, "-0 === 0");
ok(1 / -0 === -Infinity, "sign of -0 preserved via 1/x");
ok(0.1 + 0.2 === 0.30000000000000004, "double addition exact");
ok(7 % 3 === 1 && -7 % 3 === -1, "modulo sign follows dividend");
ok(parseInt("42") === 42 && parseInt("x") !== parseInt("x"), "parseInt basics");
ok("42" === 42 + "" , "number->string int");
ok(String(1.5) === "1.5", "String(1.5)");
ok(String(-0) === "0", "String(-0) = 0");

// strict equality
let o1 = {a: 1}, o2 = {a: 1};
ok(o1 === o1 && o1 !== o2, "object identity");
ok("a" === "a", "string identity");
ok(o1 === o1 && !(o1 !== o1), "strict ne negation");

// closures + captured state
function counter() {
  let n = 0;
  return function () { n += 1; return n; };
}
let c1 = counter(), c2 = counter();
ok(c1() === 1 && c1() === 2 && c2() === 1, "independent closure cells");

// context chain depth 2
function lvl1() {
  let a = 10;
  return function () {
    return function () { return a + 1; };
  };
}
ok(lvl1()()() === 11, "nested closure depth 2");

// TDZ
let tdzOk = false;
try { let z = z; } catch (e) { tdzOk = true; }
ok(tdzOk, "let self-ref in initializer throws TDZ (ES §13.3.1)");
function tdzFn() { return q; let q = 1; }
let threw = false;
try { tdzFn(); } catch (e) { threw = true; }
ok(threw, "TDZ read before initialization throws");

// const
const K = 5;
ok(K === 5, "const binding");

// objects / shapes
let obj = {};
obj.x = 1; obj.y = 2; obj.z = 3;
ok(obj.x === 1 && obj.y === 2 && obj.z === 3, "shape transitions store correctly");
delete obj.y;
ok(obj.y === undefined && obj.x === 1, "delete keeps siblings");
obj.y = 9;
ok(obj.y === 9, "re-add after delete");
let key = "dyn";
ok(obj[key] === undefined && obj["x"] === 1, "string keyed access");
ok("x" in obj && !("nope" in obj), "in operator");
ok(obj.hasOwnProperty("x") && !obj.hasOwnProperty("toString"), "hasOwnProperty");

// arrays
let arr = [1, 2, 3];
arr.push(4);
ok(arr.length === 4 && arr[3] === 4, "push + length");
ok(arr.pop() === 4 && arr.length === 3, "pop");
let sum = 0;
for (let i = 0; i < arr.length; i++) sum += arr[i];
ok(sum === 6, "for loop array sum");
let fsum = 0;
arr.forEach(function (v) { fsum += v; });
ok(fsum === 6, "forEach");
let doubled = arr.map(function (v) { return v * 2; });
ok(doubled[0] === 2 && doubled[2] === 6, "map");
ok(arr.indexOf(2) === 1 && arr.indexOf(99) === -1, "indexOf");
ok(arr.includes(3) && !arr.includes(30), "includes");
let parts = arr.join("-");
ok(parts === "1-2-3", "join");
ok(arr.slice(1)[0] === 2 && arr.slice(1).length === 2, "slice");

// strings
let s = "hello";
ok(s.length === 5, "string length");
ok(s.charAt(1) === "e" && s.charCodeAt(1) === 101, "charAt/charCodeAt");
ok(s.indexOf("ll") === 2 && s.indexOf("z") === -1, "indexOf");
ok(s.slice(1, 3) === "el", "slice");
ok(s.substring(1, 3) === "el", "substring");
ok(s.split("l")[0] === "he" && s.split("l").length === 3, "split");
ok(s + " " + "world" === "hello world", "concat");
let tpl = `n=${1 + 2}!`;
ok(tpl === "n=3!", "template literal");
ok(s[1] === "e", "string index access");

// coercion
ok(1 + "1" === "11", "number + string");
ok("5" * 2 === 10, "string * number");
ok(1 == "1" && 1 !== "1", "abstract vs strict eq");
ok(null == undefined && null !== undefined, "null == undefined");
ok(true + 1 === 2, "boolean + number");
ok(!0 === true && !1 === false, "logical not");
ok(!!"a" === true && !!"" === false, "boolean coercion of strings");
ok("" == 0, "empty string coerces to 0");

// logic / control
ok((1 && 2) === 2 && (0 && 2) === 0, "&& value semantics");
ok((0 || 3) === 3 && (1 || 3) === 1, "|| value semantics");
ok((null ?? 5) === 5 && (0 ?? 5) === 0, "?? semantics");
ok(true ? 1 : 2 === 1, "ternary");
let sw = 0;
while (sw < 3) sw++;
ok(sw === 3, "while");
let dw = 0;
do { dw++; } while (dw < 3);
ok(dw === 3, "do-while");
let brk = 0;
for (let i = 0; i < 10; i++) { if (i === 4) break; brk = i; }
ok(brk === 3, "break");
let cont = 0;
for (let i = 0; i < 5; i++) { if (i % 2) continue; cont++; }
ok(cont === 3, "continue");

// for-in
let fiKeys = "";
let fiObj = {a: 1, b: 2};
for (let k in fiObj) fiKeys += k;
ok(fiKeys === "ab", "for-in over object");
let fiArr = "";
for (let k in [10, 20]) fiArr += k + ",";
ok(fiArr === "0,1,", "for-in over array");

// exceptions
let caught = 0;
try { throw {v: 42}; } catch (e) { caught = e.v; }
ok(caught === 42, "throw + catch value");
function thrower() { throw new Error("boom"); }
let msg = "";
try { thrower(); } catch (e) { msg = e.message; }
ok(msg === "boom", "error through call frames");
let finallyRan = 0;
try { throw 1; } catch (e) { } finally { finallyRan = 1; }
ok(finallyRan === 1, "finally after catch");
function retFinally() { try { return 1; } finally { finallyRan = 2; } }
ok(retFinally() === 1 && finallyRan === 2, "return through finally");
let rethrow = 0;
try { try { throw "x"; } finally { rethrow = 1; } } catch (e) { rethrow += 1; }
ok(rethrow === 2, "finally rethrows");
let nested = 0;
try { try { throw 7; } catch (e) { throw e + 1; } } catch (e) { nested = e; }
ok(nested === 8, "rethrow in catch");

// typeof
ok(typeof 1 === "number" && typeof "s" === "string" && typeof true === "boolean");
ok(typeof undefined === "undefined" && typeof null === "object");
function f1() {}
ok(typeof f1 === "function");

// instanceof / constructors
function Point(x, y) { this.x = x; this.y = y; }
Point.prototype.mag = function () { return Math.sqrt(this.x * this.x + this.y * this.y); };
let p = new Point(3, 4);
ok(p.x === 3 && p.y === 4, "constructor fields");
ok(p.mag() === 5, "prototype method");
ok(p instanceof Point, "instanceof");

// bitwise
ok((5 & 3) === 1 && (5 | 3) === 7 && (5 ^ 3) === 6);
ok(1 << 4 === 16 && -16 >> 2 === -4 && -16 >>> 28 === 15);
ok(~5 === -6);

// inc/dec
let inc = 5;
ok(inc++ === 5 && inc === 6 && ++inc === 7 && inc-- === 7 && --inc === 5, "inc/dec order");

// misc
let big = 9007199254740992; // 2^53 — stays double
ok(big + 1 === big, "2^53 rounding");
ok(Number("3.14") === 3.14, "Number()");

print(pass + " passed, " + fail + " failed");
if (fail > 0) throw new Error(fail + " semantic failures");
