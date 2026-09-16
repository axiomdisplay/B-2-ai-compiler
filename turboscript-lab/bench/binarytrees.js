// binary trees: allocation + GC pressure (classic shootout bench, function style)
function TreeNode(left, right, item) {
  this.left = left; this.right = right; this.item = item;
}
function itemCheck(node) {
  if (node.left === null) return node.item;
  return node.item + itemCheck(node.left) - itemCheck(node.right);
}
function bottomUpTree(depth) {
  if (depth > 0) return new TreeNode(bottomUpTree(depth - 1), bottomUpTree(depth - 1), depth);
  return new TreeNode(null, null, depth);
}
let minDepth = 4, maxDepth = 14;
let stretchDepth = maxDepth + 1;
let check = itemCheck(bottomUpTree(stretchDepth));
print("stretch check: " + check);
let longCheck = itemCheck(bottomUpTree(minDepth));
for (let depth = minDepth; depth <= maxDepth; depth += 2) {
  let iterations = 1 << (maxDepth - depth + minDepth);
  check = 0;
  for (let i = 0; i < iterations; i++) {
    check += itemCheck(bottomUpTree(depth));
  }
  longCheck += check;
}
print("checksum: " + longCheck);
