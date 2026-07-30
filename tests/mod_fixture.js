export const PI = 3.14159;
export let count = 0;
export function inc() { count = count + 1; return count; }
export class Greeter { hello() { return "hi"; } }
export default function () { return "default-export"; }
export const { a, b } = { a: 1, b: 2 };
