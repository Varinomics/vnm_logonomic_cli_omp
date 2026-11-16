import type { ExtensionContext } from "@oh-my-pi/pi-coding-agent";

export type NativeCatalogEntry = {
    provider: string;
    model: string;
    display_name?: string;
    reasoning?: boolean;
};

export type NativeCatalog = {
    state: "available" | "unavailable";
    models: NativeCatalogEntry[];
    total_count: number;
    truncated: boolean;
};

// The helper owns a 32 KiB input-line bound. Leave space for activation and
// event framing without changing the lifecycle transport's established bound.
const maximumBytes = 24 * 1024;
const maximumModels = 128;
const maximumNativeModels = 16384;
const unavailable = (): NativeCatalog => ({ state: "unavailable", models: [], total_count: 0, truncated: false });
const text = (value: unknown, maximum: number): value is string => typeof value === "string" &&
    value.length > 0 && Buffer.byteLength(value) <= maximum && Buffer.from(value).toString("utf8") === value &&
    !/[\x00-\x1f\x7f]/u.test(value);

/** Native configured availability, not billing access or the enabledModels picker filter. */
export function projectNativeCatalog(value: unknown): NativeCatalog {
    if (!Array.isArray(value) || value.length > maximumNativeModels) return unavailable();
    const identities = new Set<string>();
    const records: NativeCatalogEntry[] = [];
    for (const model of value) {
        if (!model || typeof model !== "object" || !text(model.provider, 512) || !text(model.id, 512)) return unavailable();
        const identity = JSON.stringify([model.provider, model.id]);
        if (identities.has(identity)) return unavailable();
        identities.add(identity);
        records.push({ provider: model.provider, model: model.id,
            ...(text(model.name, 256) ? { display_name: model.name } : {}),
            ...(typeof model.reasoning === "boolean" ? { reasoning: model.reasoning } : {}) });
    }
    records.sort((a, b) => a.provider < b.provider ? -1 : a.provider > b.provider ? 1 :
        a.model < b.model ? -1 : a.model > b.model ? 1 : 0);
    const result: NativeCatalog = { state: "available", models: [], total_count: records.length, truncated: false };
    for (const model of records) {
        if (result.models.length === maximumModels) break;
        result.models.push(model);
        result.truncated = result.models.length < records.length;
        if (Buffer.byteLength(JSON.stringify(result)) > maximumBytes) {
            result.models.pop();
            break;
        }
    }
    result.truncated = result.models.length < result.total_count;
    return result;
}

/** The caller owns activation and cancellation; this never opens a model request. */
export async function sampleNativeCatalog(ctx: ExtensionContext): Promise<NativeCatalog> {
    try {
        if (typeof ctx.models?.list !== "function") return unavailable();
        // This is existing native background work. The caller schedules this
        // read outside lifecycle delivery and drops a retired activation.
        if (typeof ctx.modelRegistry?.awaitBackgroundRefresh === "function") {
            await ctx.modelRegistry.awaitBackgroundRefresh();
        }
        return projectNativeCatalog(ctx.models.list());
    } catch { return unavailable(); }
}
