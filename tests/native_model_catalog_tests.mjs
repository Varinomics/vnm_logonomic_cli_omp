import assert from "node:assert/strict";
import { projectNativeCatalog, sampleNativeCatalog } from "../package/assets/native_model_catalog.ts";

const native = [{ provider: "native-provider", id: "actual-model", name: "Native model", reasoning: true,
    apiKey: "must-not-leak", headers: { Authorization: "must-not-leak" }, cost: { input: 99 } }];
assert.deepEqual(projectNativeCatalog(native), { state: "available", models: [{ provider: "native-provider",
    model: "actual-model", display_name: "Native model", reasoning: true }], total_count: 1, truncated: false });
assert.deepEqual(projectNativeCatalog([]), { state: "available", models: [], total_count: 0, truncated: false });
assert.equal(projectNativeCatalog([native[0], native[0]]).state, "unavailable");
assert.equal(projectNativeCatalog([{ provider: "invalid\nprovider", id: "model" }]).state, "unavailable");
assert.equal(projectNativeCatalog([{ provider: "provider", id: "" }]).state, "unavailable");
assert.deepEqual(projectNativeCatalog([{ provider: "provider", id: "model", reasoning: "high" }]).models,
    [{ provider: "provider", model: "model" }]);
const large = Array.from({ length: 200 }, (_, index) => ({ provider: "provider", id: `model-${String(index).padStart(3, "0")}`,
    name: "Native model ".repeat(20), reasoning: false }));
const bounded = projectNativeCatalog(large);
assert.equal(bounded.state, "available");
assert.equal(bounded.total_count, 200);
assert.equal(bounded.truncated, true);
assert(bounded.models.length > 0 && bounded.models.length <= 128);
assert(Buffer.byteLength(JSON.stringify(bounded)) <= 24 * 1024);
assert.deepEqual(projectNativeCatalog([...large].reverse()), bounded);
let finish;
let queried = false;
const read = sampleNativeCatalog({ models: { list() { queried = true; return native; } },
    modelRegistry: { awaitBackgroundRefresh: () => new Promise(resolve => { finish = resolve; }) } });
assert.equal(queried, false);
finish();
assert.equal((await read).models[0].model, "actual-model");
assert.equal(queried, true);
assert.equal((await sampleNativeCatalog({ models: { list() { throw new Error("private native error"); } } })).state,
    "unavailable");
assert.equal(projectNativeCatalog([{ provider: "fixture", id: "\ud800" }]).state, "unavailable");
console.log("Native catalog scope, projection, completeness and byte bounds passed");
