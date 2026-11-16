#!/usr/bin/env python3
"""Qualify native in-memory fork/discard records through the real usage adapter.

The official SessionManager authors and transforms fixture accounting entries.
This does not patch FileLock, persist a native session, or make a model request.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--host-simulator", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    require(os.name == "posix", "This native gate uses the Linux Bun fixture")
    output = args.artifacts.resolve()
    output.mkdir(parents=True, exist_ok=False)
    output.chmod(0o700)
    runtime = args.runtime.resolve() / "node_modules"
    bun = runtime / "@oven/bun-linux-x64/bin/bun"
    native = runtime / "@oh-my-pi/pi-coding-agent/src/session/session-manager.ts"
    cli = runtime / "@oh-my-pi/pi-coding-agent/dist/cli.js"
    package, simulator = args.package.resolve(), args.host_simulator.resolve()
    home, cwd, profile = output / "home", output / "workspace", output / "host-profile"
    sources = home / ".omp/agent/sessions/project"
    for path in (sources, cwd, profile, output / "bin"):
        path.mkdir(parents=True)
    launcher = output / "bin/omp"
    launcher.write_text(f"#!/bin/sh\nexec {shlex.quote(str(bun))} {shlex.quote(str(cli))} \"$@\"\n")
    launcher.chmod(0o700)
    environment = {"HOME": str(home), "PATH": str(output / "bin") + ":/usr/bin:/bin"}
    probe = output / "native_probe.ts"
    probe.write_text("import { SessionManager } from " + json.dumps(str(native)) + ";\n" + r'''
const session = SessionManager.inMemory(process.cwd());
const usage = {input:60,output:30,cacheRead:10,cacheWrite:0,totalTokens:100,
    cost:{input:0,output:0,cacheRead:0,cacheWrite:0,total:0}};
session.appendMessage({role:"user",content:"Native accounting fixture",timestamp:Date.now()});
const original = session.appendMessage({role:"assistant",content:[{type:"text",text:"Recorded fixture"}],
    provider:"fixture",model:"local-model",api:"openai-completions",responseId:"native-source-response",
    usage,stopReason:"stop",timestamp:Date.now()});
const capture = () => structuredClone({header:session.getHeader(),entries:session.getEntries()});
const before = capture();
session.createBranchedSession(original);
const fork = capture();
await session.discardEntryDurably(original);
const after = capture();
const auxiliary = session.appendModelUsage({purpose:"fixture-auxiliary",provider:"fixture",model:"local-model",
    api:"openai-completions",usage:{...usage,input:7,output:0,cacheRead:0,totalTokens:7},stopReason:"stop"},
    {sessionId:session.getSessionId(),parentId:session.getLeafId()});
console.log(JSON.stringify({original,auxiliary,before,fork,after,after_aux:capture()}));
''')
    run = subprocess.run([str(bun), str(probe)], cwd=cwd, env=environment,
                         capture_output=True, text=True, timeout=20)
    (output / "native.log").write_text(run.stderr)
    require(run.returncode == 0, "The supported native in-memory transformation failed")
    captured = json.loads(run.stdout)
    (output / "native-entries.json").write_text(json.dumps(captured, indent=2) + "\n")
    original = next(entry for entry in captured["before"]["entries"] if entry["id"] == captured["original"])
    copied = next(entry for entry in captured["fork"]["entries"] if entry["id"] == captured["original"])
    replacement = next(entry for entry in captured["after"]["entries"] if entry["type"] == "model_usage")
    marker = next(entry for entry in captured["after"]["entries"] if entry["type"] == "branch_summary")
    require(original == copied, "Native branch copying did not preserve the billed source entry")
    require(replacement["id"] != original["id"] and replacement["timestamp"] == original["timestamp"] and
            replacement["purpose"] == "discarded-turn" and "responseId" not in replacement and
            marker["details"]["discardedEntryId"] == original["id"] and
            marker["parentId"] == replacement["parentId"] == replacement["activeLeafId"],
            "The native discard oracle no longer establishes the supported alias")

    def invoke(capability, version, operation, payload, name):
        script = {"script_version": 1, "steps": [
            {"op": "invoke", "id": "request", "capability_id": capability, "version": version,
             "operation": operation, "payload": payload},
            {"op": "wait_result", "id": "request", "timeout_ms": 10000}, {"op": "shutdown"}]}
        path = output / f"{name}.json"
        path.write_text(json.dumps(script, indent=2) + "\n")
        result = subprocess.run([str(simulator), str(package), "--script", str(path),
                                 "--platform", "linux-x86_64"], capture_output=True, text=True, timeout=20)
        (output / f"{name}.log").write_text(result.stdout + result.stderr)
        require(result.returncode == 0, f"The actual SDK invocation failed: {name}")
        frames = [json.loads(line[2:]) for line in result.stdout.splitlines() if line.startswith("< ")]
        response = next(frame for frame in frames if frame.get("type") == "result")
        require(response["status"] == "success", f"Adapter {name} failed: {response}")
        return response["payload"]

    accounts_request = {"launcher": {"script_body": "omp", "runner_key": "direct", "environment_text": ""},
        "launch_environment": {"platform": "linux-x86_64", "effective_environment": environment},
        "normalization_directory": str(cwd), "default_home_fallback": str(home)}
    inventory = invoke("accounts", 2, "discover", accounts_request, "accounts-discover")
    selected = invoke("accounts", 2, "resolve", accounts_request, "accounts-resolve")
    account = next(item for item in inventory["accounts"] if item["adapter_account_key"] == selected["adapter_account_key"])
    context = {"launcher": accounts_request["launcher"], "launcher_argv": ["omp"], "working_directory": str(cwd),
        "launch_environment": accounts_request["launch_environment"], "declared_assets": [],
        "profile_storage": {"version": 1, "root": str(profile)},
        "account_context": {"version": 2, "adapter_account_key": account["adapter_account_key"],
                            "profile_path": account["profile_path"]}}

    def write_native(path, capture):
        # Serialize exactly what the public native manager returned. The
        # harness supplies storage; it does not manufacture entry identities.
        path.write_text("".join(json.dumps(record) + "\n" for record in [capture["header"], *capture["entries"]]))

    def scan(name, accepted, count, total, mode="incremental"):
        candidate = profile / name
        candidate.mkdir()
        request = {**context, "mode": mode, "snapshot_id": None, "cursor": None,
            "cache_selection": {"version": 1, "accepted": accepted, "candidate_root": str(candidate)}}
        page = invoke("usage-scan", 3, "scan", request, name)
        require(page["complete"] and page["next_cursor"] is None and page["incomplete_historical_coverage"],
                "Native limited history must finish paging without claiming full lifetime coverage")
        require(page["account_context"] == context["account_context"] and
                page["profile_storage"] == context["profile_storage"], "Exact source custody was not acknowledged")
        require(len(page["records"]) == count and sum(record["total_tokens"] for record in page["records"]) == total,
                f"Wrong native deduplication/retention result in {name}")
        cache = page["cache_candidate"]
        require(cache["root"] == str(candidate) and cache["snapshot_id"] == page["snapshot_id"], "Candidate identity changed")
        manifest_path = candidate / "cache-manifest.json"
        require(digest(manifest_path) == cache["manifest_sha256_hex"], "Candidate manifest was not sealed before return")
        manifest = json.loads(manifest_path.read_text())
        require(manifest["format"] == "omp-native-usage@1" and len(manifest["files"]) == 1,
                "Unexpected native ledger format")
        for file in manifest["files"]:
            path = candidate / file["path"]
            require(path.stat().st_size == file["bytes"] and digest(path) == file["sha256_hex"], "Ledger seal mismatch")
        for record in page["records"]:
            proof = record["capabilities"]["usage-cross-source-event"]
            require(proof["namespace"] == "oh-my-pi.entry-usage" and record["aggregate_evidence"] == "complete",
                    "An atomic native call lacks its event proof")
        return page

    write_native(sources / "original.jsonl", captured["before"])
    write_native(sources / "fork.jsonl", captured["fork"])
    first = scan("copy", None, 1, 100)
    original_record = first["records"][0]
    require(json.loads(original_record["capabilities"]["usage-cross-source-event"]["event_id"]) ==
            [original["id"], original["timestamp"]], "Proof must use original native identity")
    write_native(sources / "fork.jsonl", captured["after_aux"])
    rewritten = scan("rewrite", first["cache_candidate"], 2, 107)
    kept = next(record for record in rewritten["records"] if record["record_id"] == original_record["record_id"])
    require(kept["total_tokens"] == 100 and kept["capabilities"] == original_record["capabilities"],
            "Native discarded replacement changed the accepted billed identity")
    for path in sources.glob("*.jsonl"):
        path.unlink()
    retained = scan("retained", rewritten["cache_candidate"], 2, 107)
    require(retained["records"] == rewritten["records"], "Missing sources must retain exact accepted accounting and native proofs")
    scan("reset", None, 0, 0, "reset")
    (output / "identity.json").write_text(json.dumps({"adapter_sha256": digest(package / "adapter/omp_adapter"),
        "bun_sha256": digest(bun), "native_session_manager_sha256": digest(native),
        "host_simulator_sha256": digest(simulator), "harness_sha256": digest(Path(__file__)),
        "probe_sha256": digest(probe)}, indent=2) + "\n")
    print(json.dumps({"result": "passed", "native_transform": "inMemory/createBranchedSession/discardEntryDurably/appendModelUsage",
        "stages": {"copy": 100, "rewrite": 107, "retained": 107, "reset": 0}, "model_requests": 0}))


if __name__ == "__main__":
    main()
