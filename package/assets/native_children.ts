/** Bounded projection of native TaskToolDetails, never transcript text. */
export type NativeChildSnapshot = {
    id: string;
    parentId: string;
    depth: number;
    agent: unknown;
    state: "running" | "succeeded" | "failed" | "interrupted";
    description: unknown;
    currentTool: unknown;
    completionPercent: unknown;
    modelIdentity: unknown;
    thinkingLevel: unknown;
};

type RecordValue = Record<string, unknown>;
const record = (value: unknown): value is RecordValue => typeof value === "object" && value !== null && !Array.isArray(value);
const identity = (value: unknown): value is string => typeof value === "string" && value.length > 0 &&
    Buffer.byteLength(value) <= 256 && !/[\x00-\x1f\x7f]/u.test(value);
const positive = (value: unknown) => typeof value === "number" && Number.isSafeInteger(value) && value > 0;

export function nestedTaskSnapshots(parentId: string, progress: unknown): {
    snapshots: NativeChildSnapshot[]; loss?: string;
} {
    const snapshots = new Map<string, NativeChildSnapshot>();
    const stack = new Set<object>();
    let visited = 0;
    let loss: string | undefined;
    const nested = (source: RecordValue, parent: string, depth: number): void => {
        if (loss) return;
        const settled = record(source.extractedToolData) ? source.extractedToolData.task : undefined;
        if (settled !== undefined && !Array.isArray(settled)) { loss = "invalid_nested_snapshot"; return; }
        for (const details of settled ?? []) visitDetails(details, parent, depth);
        if (source.inflightTaskDetails !== undefined) visitDetails(source.inflightTaskDetails, parent, depth);
    };
    const visitDetails = (value: unknown, parent: string, depth: number): void => {
        if (loss) return;
        if (!record(value) || !Array.isArray(value.results) ||
            (value.progress !== undefined && !Array.isArray(value.progress))) {
            loss = "invalid_nested_snapshot"; return;
        }
        if (depth > 8 || stack.has(value)) { loss = "nested_snapshot_limit"; return; }
        stack.add(value);
        // Settled results take precedence over an older in-flight row for the
        // same native id. Empty/pending reservations never establish a child.
        for (const item of value.progress ?? []) visitItem(item, parent, depth, false);
        for (const item of value.results) visitItem(item, parent, depth, true);
        stack.delete(value);
    };
    const visitItem = (value: unknown, parent: string, depth: number, settled: boolean): void => {
        if (loss) return;
        if (++visited > 128) { loss = "nested_snapshot_limit"; return; }
        if (!record(value) || !identity(value.id) || value.id === parent) {
            loss = "invalid_nested_snapshot"; return;
        }
        // A run monitor is allocated before native session creation and can
        // fail (for example on a native file lock) without ever starting. A
        // completed request or executed tool is affirmative execution evidence.
        if (!positive(value.requests) && !positive(value.toolCount)) return;
        let state: NativeChildSnapshot["state"];
        if (settled) {
            if (value.aborted === true) state = "interrupted";
            else if (typeof value.exitCode === "number" && Number.isSafeInteger(value.exitCode))
                state = value.exitCode === 0 ? "succeeded" : "failed";
            else { loss = "invalid_nested_snapshot"; return; }
        } else {
            switch (value.status) {
                case "running": state = "running"; break;
                case "completed": state = "succeeded"; break;
                case "failed": state = "failed"; break;
                case "aborted": state = "interrupted"; break;
                case "pending": return;
                default: loss = "invalid_nested_snapshot"; return;
            }
        }
        const existing = snapshots.get(value.id);
        if (existing && (existing.parentId !== parent || existing.depth !== depth)) {
            loss = "invalid_nested_snapshot"; return;
        }
        if (!existing || existing.state === "running" || state !== "running") snapshots.set(value.id, {
            id: value.id, parentId: parent, depth, state, agent: value.agent,
            description: value.description, currentTool: value.currentTool,
            completionPercent: value.completionPercent, modelIdentity: value.resolvedModelIdentity,
            thinkingLevel: value.resolvedThinkingLevel,
        });
        nested(value, value.id, depth + 1);
    };
    if (record(progress)) nested(progress, parentId, 2);
    return loss ? { snapshots: [], loss } : { snapshots: [...snapshots.values()] };
}
