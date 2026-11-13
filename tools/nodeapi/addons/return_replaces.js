// Driver for the return-replaces probe.
if (globalThis.__exports.status === "yes") {
  print("replaced: yes");
} else {
  throw new Error("return-replaces failed: " + String(globalThis.__exports.status));
}
