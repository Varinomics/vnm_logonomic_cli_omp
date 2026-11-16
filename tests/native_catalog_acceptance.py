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
import shlex
import signal
import subprocess
import sys
import time


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def wrapper_fixture():
    """Real helper client transport, with actual packaged adapter classification."""
    descriptor = json.loads(Path(sys.argv[2]).read_text())
    arguments = sys.argv[3:]
    body = json.loads(sys.stdin.read()) if "--args-stdin" in arguments else None
    state_path = Path(descriptor["state"])
    state = json.loads(state_path.read_text()) if state_path.exists() else {
        "ledger": {"current_epoch": 0, "current_transition": None}}
    entry = {"argv": arguments, "body": body, "pid": os.getpid()}
    reply = {"accepted": True, "epoch": state["ledger"]["current_epoch"]}
    if arguments[:2] == ["provider-event", "deliver"]:
        event = body["event"]
        request = {"capability_id": "helper-ingress", "version": 2, "operation": "classify_event", "payload": {
            "event": event, "ledger": state["ledger"], "transition_nonce": body["transition_nonce"],
            "model_catalog": {"version": 1}}}
        outcome = subprocess.run([descriptor["driver"], descriptor["package"]], input=json.dumps(request),
            capture_output=True, text=True, timeout=5)
        if outcome.returncode != 0:
            raise RuntimeError("Actual packaged host classification failed: " + outcome.stderr)
        decoded = json.loads(outcome.stdout)
        assert decoded["status"] == "success", decoded
        classification = decoded["payload"]
        entry["classification"] = classification
        if classification["classification"] == "transition":
            observation = classification["observations"][0]
            state["ledger"] = {"current_epoch": 1, "current_transition": {"kind": "start",
                "provider_session_id": observation["provider_session_id"], "nonce": body["transition_nonce"]}}
            assert classification["helper_response"]["model_catalog_version"] == 1
        if event["name"] == "model_catalog":
            assert classification["classification"] == "ignore" and not classification["observations"], classification
            assert "snapshot" in classification["model_catalog"] and "failure" not in classification["model_catalog"]
            state["catalog_key"] = body["idempotency_key"]
        reply = {"accepted": True, "epoch": state["ledger"]["current_epoch"]}
        if "helper_response" in classification:
            reply["helper_response"] = classification["helper_response"]
    elif arguments[:2] == ["provider-event", "acknowledge"]:
        if len(arguments) >= 3 and arguments[2] == state.get("catalog_key"):
            state["catalog_acknowledged"] = True
    else:
        raise AssertionError("Unexpected native catalog helper operation")
    with Path(descriptor["journal"]).open("a", encoding="utf-8") as journal:
        journal.write(json.dumps(entry) + "\n")
    temporary = state_path.with_suffix(".tmp")
    write_json(temporary, state)
    temporary.replace(state_path)
    print(json.dumps(reply), flush=True)


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
    state_path = root / "helper-state.json"
    journal_path = root / "helper-journal.jsonl"
    if arguments.package:
        wrapper = root / "vnmctl-fixture"
        descriptor = root / "helper-descriptor.json"
        write_json(descriptor, {"driver": str(arguments.host_driver), "package": str(arguments.package),
            "state": str(state_path), "journal": str(journal_path)})
        wrapper.write_text("#!/bin/sh\nexec " + " ".join(shlex.quote(value) for value in
            (sys.executable, str(Path(__file__).resolve()), "--wrapper-fixture", str(descriptor))) + ' "$@"\n')
        wrapper.chmod(0o755)
        environment.update({"LOGONOMIC_VNMCTL_PATH": str(wrapper), "LOGONOMIC_SESSION_CAPABILITY": "a" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.omp", "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "b" * 64,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": "c" * 32,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(arguments.package / "helpers")})
        command.extend(["--extension", str(arguments.package / "assets/logonomic.ts")])
    write_json(root / "invocation.json", {"argv": command, "environment": environment})
    process = subprocess.Popen(command, cwd=root / "workspace", env=environment, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    streams = {"stdout": bytearray(), "stderr": bytearray()}
    pending = bytearray()
    ready = False
    input_closed = False
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
            acknowledged = state_path.exists() and json.loads(state_path.read_text()).get("catalog_acknowledged")
            if ready and not input_closed and (not arguments.package or acknowledged):
                process.stdin.close()
                input_closed = True
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
    if arguments.package:
        journal = [json.loads(line) for line in journal_path.read_text().splitlines()]
        delivered = [entry for entry in journal if entry["argv"][:2] == ["provider-event", "deliver"]]
        names = [entry["body"]["event"]["name"] for entry in delivered]
        assert names[0] == "session_start" and names[-1] == "session_shutdown", names
        assert "logonomic_observation_lost" not in names, names
        catalogs = [entry for entry in delivered if entry["body"]["event"]["name"] == "model_catalog"]
        assert len(catalogs) == 1, names
        wire = catalogs[0]["classification"]["model_catalog"]["snapshot"]
        assert all(wire[key] == catalog[key] for key in ("state", "models", "total_count", "truncated")), wire
        assert wire["provider_session_id"] == delivered[0]["body"]["event"]["body"]["session_id"]
        assert wire["transition_nonce"] == delivered[0]["body"]["transition_nonce"]
        assert json.loads(state_path.read_text()).get("catalog_acknowledged"), "Real helper did not acknowledge the catalog"
        result.update({"actual_helper_and_adapter": True, "catalog_acknowledged": True,
            "catalog_sequence": wire["sequence"], "native_event_names": names})
    write_json(root / "result.json", result)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--module", type=Path)
    parser.add_argument("--package", type=Path, help="Also run the packaged production extension and helper")
    parser.add_argument("--host-driver", type=Path, help="omp_native_host_driver for actual packaged classification")
    arguments = parser.parse_args()
    arguments.runtime = arguments.runtime.resolve()
    arguments.artifacts = arguments.artifacts.resolve()
    if bool(arguments.package) != bool(arguments.host_driver):
        parser.error("--package and --host-driver must be supplied together")
    if arguments.package:
        arguments.package = arguments.package.resolve()
        arguments.host_driver = arguments.host_driver.resolve()
    arguments.module = (arguments.module or
        (arguments.package / "assets/native_model_catalog.ts" if arguments.package else
         Path(__file__).resolve().parents[1] / "package/assets/native_model_catalog.ts")).resolve()
    results = {name: run_case(arguments, name, count) for name, count in (("scoped-profile", 3), ("bounded-native", 200))}
    write_json(arguments.artifacts / "result.json", results)
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--wrapper-fixture":
        wrapper_fixture()
    else:
        main()
