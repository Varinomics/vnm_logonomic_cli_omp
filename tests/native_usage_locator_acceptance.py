#!/usr/bin/env python3
"""Replay unchanged native-authored JSONL through exact custom-file locators.

Supply the 100-token source from native_usage_acceptance.py or its original
extensionless omission reproduction. This invokes the actual packaged adapter,
not a native model or SessionManager; the earlier capture owns native provenance.
The installed public applyStartupCwd helper separately qualifies HOME relocation.
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


def direct_command(arguments):
    return " ".join('"' + item.replace('"', '"""') + '"' for item in arguments)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--native-jsonl", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--host-simulator", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    require(os.name == "posix", "This focused packaged gate uses the Linux fixture")
    output = args.artifacts.resolve()
    output.mkdir(parents=True, exist_ok=False)
    output.chmod(0o700)
    source = args.native_jsonl.resolve()
    native_bytes = source.read_bytes()
    entries = [json.loads(line) for line in native_bytes.splitlines()]
    bills = [entry for entry in entries if entry.get("type") == "message" and
             entry.get("message", {}).get("role") == "assistant"]
    require(entries[0].get("type") == "session" and entries[0].get("version") == 3 and
            len(bills) == 1 and bills[0]["message"]["usage"]["totalTokens"] == 100,
            "Use the unchanged native 100-token omission fixture")
    (output / "unchanged-native.jsonl").write_bytes(native_bytes)
    runtime = args.runtime.resolve() / "node_modules"
    bun = runtime / "@oven/bun-linux-x64/bin/bun"
    cli = runtime / "@oh-my-pi/pi-coding-agent/dist/cli.js"
    startup = runtime / "@oh-my-pi/pi-coding-agent/src/cli/startup-cwd.ts"
    package, simulator = args.package.resolve(), args.host_simulator.resolve()
    binary = output / "bin"
    binary.mkdir()
    launcher = binary / "omp"
    launcher.write_text(f"#!/bin/sh\nexec {shlex.quote(str(bun))} {shlex.quote(str(cli))} \"$@\"\n")
    launcher.chmod(0o700)
    probe = output / "native_startup_probe.ts"
    probe.write_text("import { applyStartupCwd } from " + json.dumps(str(startup)) + ";\n"
        "const args=JSON.parse(process.argv[2]); const before=process.cwd(); "
        "await applyStartupCwd(args); console.log(JSON.stringify({before,after:process.cwd(),args}));\n")

    def invoke(case, capability, version, operation, payload, name):
        script = {"script_version": 1, "steps": [
            {"op": "invoke", "id": "request", "capability_id": capability, "version": version,
             "operation": operation, "payload": payload},
            {"op": "wait_result", "id": "request", "timeout_ms": 10000}, {"op": "shutdown"}]}
        path = case / f"{name}.json"
        path.write_text(json.dumps(script, indent=2) + "\n")
        result = subprocess.run([str(simulator), str(package), "--script", str(path),
                                 "--platform", "linux-x86_64"], capture_output=True, text=True, timeout=20)
        (case / f"{name}.log").write_text(result.stdout + result.stderr)
        require(result.returncode == 0, f"SDK invocation failed: {case.name}/{name}")
        frames = [json.loads(line[2:]) for line in result.stdout.splitlines() if line.startswith("< ")]
        return next(frame for frame in frames if frame.get("type") == "result")

    results = {}
    cases = (
        ("registry_extensionless", "registry", "external/work", 100),
        ("registry_custom", "registry", "external/work.history", 100),
        ("registry_jsonl_control", "registry", "external/work.jsonl", 100),
        ("explicit_absolute", "absolute", "external/work", 100),
        ("explicit_relative", "relative", "external/work", 100),
        ("explicit_unicode", "absolute", "external/caf\u00e9.record", 100),
        ("explicit_startup_cwd", "startup_cwd", "external/work", 100),
        ("selector_name", "selector", "workspace/work", 0),
        ("selector_uuid", "selector", "workspace/8fc4094a-6aa6-440e-b478-a5b2de576c48", 0),
        ("directory_custom", "directory", "home/.omp/agent/sessions/project/work", 0),
        ("implicit_home", "home", "home/tmp/work", 100),
        ("home_relative_directory", "home_dir", "home/tmp/history/work.jsonl", 100),
        ("home_ambient_directory", "home_env_dir", "home/tmp/history/work.jsonl", 100),
        ("home_allow", "home_allow", "home/work", 100),
        ("home_explicit_cwd", "home_cwd", "home/work", 100),
        ("home_empty_cwd", "home_empty_cwd", "home/tmp/work", 100),
        ("home_absolute", "home_absolute", "home/work", 100),
        ("malformed_exact", "malformed", "external/work", None),
    )
    for name, mode, spelling, expected in cases:
        case = output / name
        home, cwd, storage = case / "home", case / "workspace", case / "host-profile"
        for path in (home / ".omp/agent", cwd / "other", storage / "candidate"):
            path.mkdir(parents=True)
        if mode.startswith("home"):
            cwd = home
            (home / "tmp").mkdir()
        exact = case / spelling
        exact.parent.mkdir(parents=True, exist_ok=True)
        exact.write_bytes(native_bytes if mode != "malformed" else b'{"type":"unsupported"}\n')
        arguments = ["omp"]
        if mode in ("registry", "malformed"):
            registry = home / ".omp/agent/custom-session-files"
            registry.mkdir()
            (registry / "native-exact-path").write_text(str(exact))
        elif mode == "absolute":
            arguments += ["--session", str(exact)]
        elif mode == "relative":
            arguments += ["--session", "../external/work"]
        elif mode == "startup_cwd":
            arguments += ["--cwd", "./other", "--session", "../../external/work"]
        elif mode == "selector":
            arguments += ["--session", exact.name]
        elif mode == "home":
            arguments += ["--session", "./work"]
            (home / "work").write_text('{"type":"wrong-launch-home-source"}\n')
        elif mode == "home_dir":
            arguments += ["--session-dir", "./history"]
        elif mode == "home_allow":
            arguments += ["--allow-home", "--session", "./work"]
        elif mode == "home_cwd":
            arguments += ["--cwd", str(home), "--session", "./work"]
        elif mode == "home_empty_cwd":
            arguments += ["--cwd", "", "--session", "./work"]
        elif mode == "home_absolute":
            arguments += ["--session", str(exact)]
        if name == "registry_custom":
            # A custom suffix does not authorize a guessed public artifact root.
            unrelated = exact.with_suffix("") / "unrelated.jsonl"
            unrelated.parent.mkdir()
            unrelated.write_text('{"type":"unsupported"}\n')
        environment = {"HOME": str(home), "PATH": str(binary) + ":/usr/bin:/bin"}
        if mode == "home_env_dir":
            environment["PI_CODING_AGENT_SESSION_DIR"] = "./history"
        native_startup = None
        if mode.startswith("home"):
            parsed = {"allowHome": True} if mode == "home_allow" else {"cwd": str(home)} if mode == "home_cwd" else \
                {"cwd": ""} if mode == "home_empty_cwd" else {}
            native = subprocess.run([str(bun), str(probe), json.dumps(parsed)], cwd=cwd, env=environment,
                                    capture_output=True, text=True, timeout=20)
            (case / "native-startup.log").write_text(native.stdout + native.stderr)
            require(native.returncode == 0, f"Actual native startup cwd failed: {name}")
            native_startup = json.loads(native.stdout)
            expected_cwd = home if mode in ("home_allow", "home_cwd") else home / "tmp"
            require(Path(native_startup["after"]) == expected_cwd, f"The native cwd oracle changed: {name}")
        accounts_request = {"launcher": {"script_body": direct_command(arguments),
            "runner_key": "direct", "environment_text": ""},
            "launch_environment": {"platform": "linux-x86_64", "effective_environment": environment},
            "normalization_directory": str(cwd), "default_home_fallback": str(home)}
        inventory = invoke(case, "accounts", 2, "discover", accounts_request, "accounts-discover")
        selected = invoke(case, "accounts", 2, "resolve", accounts_request, "accounts-resolve")
        account = None
        if isinstance(expected, str):
            require(all(item["status"] != "success" and item.get("reason_code") == expected for item in (inventory, selected)),
                    f"Ambiguous native cwd was not refused by profile inspection: {name}")
        else:
            require(inventory["status"] == selected["status"] == "success", f"Exact profile resolution failed: {name}")
            account = next(item for item in inventory["payload"]["accounts"]
                           if item["adapter_account_key"] == selected["payload"]["adapter_account_key"])
        context = {"launcher": accounts_request["launcher"], "launcher_argv": arguments,
            "working_directory": str(cwd), "launch_environment": accounts_request["launch_environment"],
            "declared_assets": [], "profile_storage": {"version": 1, "root": str(storage)}}
        if account:
            context["account_context"] = {"version": 2, "adapter_account_key": account["adapter_account_key"],
                                          "profile_path": account["profile_path"]}
        response = invoke(case, "usage-scan", 3, "scan", {**context,
            "mode": "reset", "snapshot_id": None, "cursor": None,
            "cache_selection": {"version": 1, "accepted": None, "candidate_root": str(storage / "candidate")}}, "usage")
        if expected is None or isinstance(expected, str):
            reason = expected if expected is not None else "usage_native_format_unsupported"
            require(response["status"] != "success" and response.get("reason_code") == reason,
                    f"Unsupported exact source was not refused: {response}")
            results[name] = {"status": response["status"], "reason_code": response["reason_code"]}
            continue
        require(response["status"] == "success", f"Exact source scan failed: {name}: {response}")
        page = response["payload"]
        tokens = sum(record["total_tokens"] for record in page["records"])
        require(page["complete"] and page["next_cursor"] is None and page["incomplete_historical_coverage"] and
                tokens == expected and len(page["records"]) == int(expected != 0), f"Wrong exact-source count: {name}")
        require(page["account_context"] == context["account_context"] and page["profile_storage"] == context["profile_storage"],
                f"Selected source custody changed: {name}")
        cache = page["cache_candidate"]
        manifest_path = storage / "candidate/cache-manifest.json"
        require(cache["root"] == str(storage / "candidate") and digest(manifest_path) == cache["manifest_sha256_hex"],
                f"Unsealed exact-source candidate: {name}")
        for item in json.loads(manifest_path.read_text())["files"]:
            retained = storage / "candidate" / item["path"]
            require(retained.stat().st_size == item["bytes"] and digest(retained) == item["sha256_hex"],
                    f"Exact-source ledger seal mismatch: {name}")
        results[name] = {"records": len(page["records"]), "tokens": tokens,
                         "complete": page["complete"], "incomplete_historical_coverage": True,
                         "native_bytes_unchanged": exact.read_bytes() == native_bytes}
        if native_startup:
            results[name]["native_startup"] = native_startup
    (output / "identity.json").write_text(json.dumps({"native_jsonl": str(source), "native_jsonl_sha256": digest(source),
        "adapter_sha256": digest(package / "adapter/omp_adapter"), "host_simulator_sha256": digest(simulator),
        "harness_sha256": digest(Path(__file__)), "native_startup_cwd_sha256": digest(startup),
        "bun_sha256": digest(bun)}, indent=2) + "\n")
    summary = {"result": "passed", "cases": results, "model_requests": 0,
               "native_startup_helper_invocations": sum("native_startup" in result for result in results.values())}
    (output / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
