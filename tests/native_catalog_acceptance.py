#!/usr/bin/env python3
"""Run the production catalog sampler in an isolated official OMP process.

No prompt or paid provider is used. Every model belongs to a local keyless
fixture, and the native process inherits only the explicit fixture environment.
"""

import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def run_case(arguments, name, count):
    root = arguments.artifacts / name
    root.mkdir(parents=True, exist_ok=False)
    for part in ("home", "workspace", "natives", "fallback-agent"):
        (root / part).mkdir()
    agent = root / "home/.omp-catalog/profiles/chosen/agent"
    models = [{"id": f"model-{index:03}", "name": "Native catalog model " + ("description " * 18 if count > 128 else str(index)),
               "reasoning": index % 2 == 0, "input": ["text"], "contextWindow": 32768, "maxTokens": 1024,
               "cost": {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0}} for index in range(count)]
    write_json(agent / "models.yml", {"providers": {"catalog-fixture": {
        "baseUrl": "http://127.0.0.1:9/v1", "auth": "none", "api": "openai-completions", "models": models}}})
    write_json(agent / "config.yml", {"enabledProviders": ["catalog-fixture"],
        "telemetry": {"otlpExportEnabled": False}, "marketplace": {"autoUpdate": "off"},
        "startup": {"setupWizard": False, "checkUpdate": False, "quiet": True},
        "power": {"sleepPrevention": "off"}})
    write_json(root / "fallback-agent/models.yml", {"providers": {"wrong-profile": {
        "baseUrl": "http://127.0.0.1:9/v1", "auth": "none", "api": "openai-completions",
        "models": [{**models[0], "id": "must-not-appear"}]}}})
    report = root / "native-observation.json"
    extension = root / "probe.ts"
    extension.write_text(
        'import { writeFile } from "node:fs/promises";\n'
        f'import {{ sampleNativeCatalog }} from {json.dumps(str(arguments.module))};\n'
        'export default function(pi) {\n'
        '  pi.on("session_start", async (_event, ctx) => {\n'
        '    const catalog = await sampleNativeCatalog(ctx);\n'
        '    const native = ctx.models.list();\n'
        '    await writeFile(process.env.CATALOG_PROBE_REPORT, JSON.stringify({catalog,\n'
        '      native_count:native.length, native_bytes:Buffer.byteLength(JSON.stringify(native)),\n'
        '      snapshot_bytes:Buffer.byteLength(JSON.stringify(catalog))}));\n'
        '  });\n}\n', encoding="utf-8")
    bun = arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"
    cli = arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent/dist/cli.js"
    environment = {"PATH": "/usr/bin:/bin", "HOME": str(root / "home"), "LANG": "C.UTF-8",
        "OMP_PROFILE": "chosen", "PI_PROFILE": "ignored", "PI_CONFIG_DIR": ".omp-catalog",
        "PI_CODING_AGENT_DIR": str(root / "fallback-agent"), "PI_NATIVES_DIR": str(root / "natives"),
        "PI_NO_TITLE": "1", "PI_NO_PTY": "1", "CATALOG_PROBE_REPORT": str(report)}
    command = [str(bun), str(cli), "--mode", "rpc", "--no-ui", "--no-session", "--offline",
        "--provider", "catalog-fixture", "--model", "model-000", "--extension", str(extension)]
    write_json(root / "invocation.json", {"argv": command, "environment": environment})
    process = subprocess.Popen(command, cwd=root / "workspace", env=environment, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    streams = {"stdout": bytearray(), "stderr": bytearray()}
    pending = bytearray()
    ready = False
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, "stdout")
    selector.register(process.stderr, selectors.EVENT_READ, "stderr")
    deadline = time.monotonic() + 30
    try:
        while selector.get_map():
            if time.monotonic() >= deadline:
                raise AssertionError("The real native catalog process exceeded its bounded lifetime")
            for key, _ in selector.select(0.1):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                streams[key.data].extend(chunk)
                if len(streams[key.data]) > 2 * 1024 * 1024:
                    raise AssertionError("The real native process exceeded its stream bound")
                if key.data == "stdout":
                    pending.extend(chunk)
                    while b"\n" in pending:
                        line, _, remainder = pending.partition(b"\n")
                        pending[:] = remainder
                        frame = json.loads(line)
                        if frame.get("type") == "ready" and not ready:
                            ready = True
                            process.stdin.close()
        assert process.wait(timeout=5) == 0, "Native OMP did not exit cleanly after EOF"
        assert ready and report.is_file(), "The actual extension loader never produced the catalog observation"
    finally:
        selector.close()
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
        for name, value in streams.items():
            (root / f"native-{name}.log").write_bytes(value)
    observed = json.loads(report.read_text())
    catalog = observed["catalog"]
    assert catalog["state"] == "available" and observed["native_count"] == count, observed
    assert catalog["total_count"] == count and catalog["truncated"] == (len(catalog["models"]) < count), observed
    assert 0 < len(catalog["models"]) <= 128 and observed["snapshot_bytes"] <= 24 * 1024, observed
    assert all(model["provider"] == "catalog-fixture" and model["model"].startswith("model-") for model in catalog["models"])
    assert all(set(model) <= {"provider", "model", "display_name", "reasoning"} for model in catalog["models"])
    helper_request = {"operation": "event", "name": "model_catalog", "body": {"session_id": "s" * 256,
        "activation_nonce": "f" * 32, "event_id": "e" * 32, "sequence": "18446744073709551615", "catalog": catalog}}
    helper_bytes = len(json.dumps(helper_request, ensure_ascii=False, separators=(",", ":")).encode()) + 1
    assert helper_bytes < 32 * 1024, "The optional catalog cannot fit the existing helper input contract"
    result = {"native_models": count, "reported_models": len(catalog["models"]), "native_bytes": observed["native_bytes"],
        "snapshot_bytes": observed["snapshot_bytes"], "helper_request_bytes": helper_bytes,
        "truncated": catalog["truncated"], "native_exit": process.returncode}
    write_json(root / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--module", type=Path, default=Path(__file__).resolve().parents[1] / "package/assets/native_model_catalog.ts")
    arguments = parser.parse_args()
    arguments.runtime = arguments.runtime.resolve()
    arguments.artifacts = arguments.artifacts.resolve()
    arguments.module = arguments.module.resolve()
    results = {name: run_case(arguments, name, count) for name, count in (("scoped-profile", 3), ("bounded-native", 200))}
    write_json(arguments.artifacts / "result.json", results)
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
