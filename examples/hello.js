// L/R_JS - Example Script
// Demonstrates ES2022 features and browser APIs

console.log("=== L/R_JS Example ===");
console.log("");

// ── ES2022 Features ─────────────────────────────────────────────────
console.log("--- ES2022 Features ---");

// Top-level await would go here, but we need --module flag

// Array.at()
console.log("Array.at():", [10, 20, 30, 40, 50].at(-1));
console.log("Array.at():", [10, 20, 30, 40, 50].at(2));

// Object.hasOwn() (ES2022)
console.log("Object.hasOwn:", typeof Object.hasOwn === 'function' ? 'available' : 'not available');

// Error.cause (ES2022)
console.log("Error.cause:", typeof Error.cause !== 'undefined' ? 'available' : 'not available');

// Class fields
class Counter {
    count = 0;
    increment() { return ++this.count; }
}
const c = new Counter();
console.log("Class fields:", c.increment(), c.increment());

// Private methods/fields (ES2022)
class Secret {
    #secret = 42;
    getSecret() { return this.#secret; }
}
const s = new Secret();
console.log("Private fields:", s.getSecret());

// ── Console API ─────────────────────────────────────────────────────
console.log("\n--- Console API ---");
console.log("log message");
console.info("info message");
console.warn("warn message");
console.error("error message");
console.debug("debug message");

// ── URL API ─────────────────────────────────────────────────────────
console.log("\n--- URL API ---");
const url = new URL("https://example.com:8080/path/to/page?key=value&foo=bar#section");
console.log("href:", url.href);
console.log("hostname:", url.hostname);
console.log("port:", url.port);
console.log("pathname:", url.pathname);
console.log("search:", url.search);
console.log("hash:", url.hash);
console.log("origin:", url.origin);

// ── TextEncoder/TextDecoder ─────────────────────────────────────────
console.log("\n--- Text Encoding ---");
const encoder = new TextEncoder();
const encoded = encoder.encode("Hello, 世界!");
console.log("Encoded length:", encoded.length);

const decoder = new TextDecoder();
const decoded = decoder.decode(encoded);
console.log("Decoded:", decoded);

// Base64
console.log("btoa:", btoa("Hello, World!"));
console.log("atob:", atob("SGVsbG8sIFdvcmxkIQ=="));

// ── Crypto API ──────────────────────────────────────────────────────
console.log("\n--- Crypto API ---");
console.log("randomUUID:", crypto.randomUUID());
console.log("randomUUID:", crypto.randomUUID());

// ── Performance API ─────────────────────────────────────────────────
console.log("\n--- Performance API ---");
performance.mark("start");
let sum = 0;
for (let i = 0; i < 1000000; i++) sum += i;
performance.mark("end");
performance.measure("loop", "start", "end");
console.log("performance.now():", performance.now());

// ── Storage API ─────────────────────────────────────────────────────
console.log("\n--- Storage API ---");
localStorage.setItem("key1", "value1");
localStorage.setItem("key2", "value2");
console.log("localStorage.getItem('key1'):", localStorage.getItem("key1"));
console.log("localStorage.length:", localStorage.length);
localStorage.removeItem("key1");
console.log("After remove, length:", localStorage.length);

// ── Event API ───────────────────────────────────────────────────────
console.log("\n--- Event API ---");
const target = new EventTarget();
target.addEventListener("test", (e) => {
    console.log("Event received:", e.type);
});
const event = new Event("test");
target.dispatchEvent(event);

// ── Timers ──────────────────────────────────────────────────────────
console.log("\n--- Timers ---");
setTimeout(() => {
    console.log("Timeout fired after 50ms");
}, 50);

let count = 0;
const intervalId = setInterval(() => {
    count++;
    console.log("Interval tick:", count);
    if (count >= 3) {
        clearInterval(intervalId);
        console.log("Interval cleared");
    }
}, 20);

// ── Promise ─────────────────────────────────────────────────────────
Promise.resolve("resolved")
    .then(v => {
        console.log("Promise resolved:", v);
        return "chained";
    })
    .then(v => console.log("Chained:", v));

// ── JSON ────────────────────────────────────────────────────────────
console.log("\n--- JSON ---");
const obj = { name: "L/R_JS", version: "1.0", features: ["fast", "lightweight", "ES2022"] };
const json = JSON.stringify(obj, null, 2);
console.log(json);
console.log("Parsed:", JSON.parse(json).name);

console.log("\n=== Example Complete ===");