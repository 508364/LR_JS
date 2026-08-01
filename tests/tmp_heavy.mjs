console.log("start heavy");
const MAP_SIZE = 5000;
function buildNested(depth) {
  let obj = { value: depth };
  for (let i = depth - 1; i >= 0; i--) {
    obj = { value: i, child: obj };
  }
  return obj;
}
console.log("building map...");
const bigMap = new Map();
for (let i = 0; i < MAP_SIZE; i++) {
  bigMap.set("key-" + i, { id: i, payload: "x".repeat(100), nested: buildNested(5) });
}
console.log("map done, size:", bigMap.size);
