#!/usr/bin/env python3
"""Opt-in native dotenv/profile boundary regression; no model or login calls."""

import argparse
import base64
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
    require(os.name == "posix", "This actual native regression requires the Linux Bun fixture")
    runtime = args.runtime.resolve() / "node_modules"
    bun = runtime / "@oven/bun-linux-x64/bin/bun"
    cli = runtime / "@oh-my-pi/pi-coding-agent/dist/cli.js"
    utils = runtime / "@oh-my-pi/pi-utils/src"
    package = args.package.resolve()
    simulator = args.host_simulator.resolve()
    output = args.artifacts.resolve()
    output.mkdir(parents=True, exist_ok=False)
    os.chmod(output, 0o700)
    binary = output / "bin"
    binary.mkdir()
    launcher = binary / "omp"
    launcher.write_text(f"#!/bin/sh\nexec {shlex.quote(str(bun))} {shlex.quote(str(cli))} \"$@\"\n")
    launcher.chmod(0o700)

    def invoke(case, capability, version, operation, payload, label):
        script = {"script_version": 1, "steps": [
            {"op": "invoke", "id": "request", "capability_id": capability,
             "version": version, "operation": operation, "payload": payload},
            {"op": "wait_result", "id": "request", "timeout_ms": 8000}, {"op": "shutdown"}]}
        path = case / f"{label}.json"
        path.write_text(json.dumps(script, indent=2) + "\n")
        run = subprocess.run([str(simulator), str(package), "--script", str(path),
                              "--platform", "linux-x86_64"], capture_output=True, text=True, timeout=15)
        (case / f"{label}.log").write_text(run.stdout + run.stderr)
        require(run.returncode == 0, f"SDK invocation failed: {case.name}/{label}")
        frames = [json.loads(line[2:]) for line in run.stdout.splitlines() if line.startswith("< ")]
        return next(frame for frame in frames if frame.get("type") == "result")

    # The first five variants are accepted selector syntax by actual Bun
    # parseEnv. The other refusals deliberately avoid comment/quote parsing.
    cases = [
        ("equals", "PI_CONFIG_DIR=.redirected\n", True, True),
        ("bom", "\ufeffPI_CONFIG_DIR=.redirected\n", True, True),
        ("colon", "PI_CONFIG_DIR : .redirected\n", True, True),
        ("vertical-whitespace", "\vPI_CONFIG_DIR\f= .redirected\n", True, True),
        ("newline-before-equals", "PI_CONFIG_DIR\n= .redirected\n", True, True),
        ("omp-alias", "OMP_CONFIG_DIR=.redirected\n", True, True),
        ("comment", "# PI_CONFIG_DIR=.redirected\n", True, False),
        ("quoted-key", '"PI_CONFIG_DIR"=.redirected\n', True, False),
        ("quoted-value", "OPENAI_API_KEY='PI_CONFIG_DIR=.redirected'\n", True, False),
        ("mixed-case", "Pi_Config_Dir=.redirected\n", True, False),
        ("credential-equals", "OPENAI_API_KEY=fixture-only\n", False, False),
        ("credential-bom-colon", "\ufeffOPENAI_API_KEY : fixture-only\n", False, False),
        ("credential-multiline", "ANTHROPIC_API_KEY='fixture\ncredential'\n", False, False),
    ]
    rows = []
    for label, content, refuse, redirected in cases:
        case = output / label
        home, cwd = case / "home", case / "workspace"
        for path in (home / ".omp/agent", home / ".redirected/agent", cwd):
            path.mkdir(parents=True)
        environment = {"HOME": str(home), "PATH": str(binary) + ":/usr/bin:/bin"}
        request = {"launcher": {"script_body": "omp", "runner_key": "direct", "environment_text": ""},
                   "launch_environment": {"platform": "linux-x86_64", "effective_environment": environment},
                   "normalization_directory": str(cwd), "default_home_fallback": str(home)}
        baseline = invoke(case, "accounts", 2, "resolve", request, "baseline-resolve")
        require(baseline["status"] == "success", "The unchanged native default profile must resolve first")
        account = {"version": 2, "adapter_account_key": baseline["payload"]["adapter_account_key"],
                   "profile_path": str(home / ".omp")}
        dotenv = home / ".env"
        dotenv.write_text(content, encoding="utf-8")
        discovery = invoke(case, "accounts", 2, "discover", request, "discover")
        resolution = invoke(case, "accounts", 2, "resolve", request, "resolve")
        prepare_request = {"purpose": "implementation", "launcher": request["launcher"],
                           "launcher_argv": ["omp"], "working_directory": str(cwd),
                           "launch_environment": request["launch_environment"],
                           "prompt_base64": base64.b64encode(b"No model invocation is performed.").decode(),
                           "declared_assets": [], "account_context": account}
        preparation = invoke(case, "structured-session", 3, "prepare", prepare_request, "prepare")
        code = (f"const dirs=await import({json.dumps(str(utils / 'dirs.ts'))});"
                "dirs.setProfile(undefined);"
                f"const env=await import({json.dumps(str(utils / 'env.ts'))});"
                "console.log(JSON.stringify({config:dirs.getConfigRootDir(),agent:dirs.getAgentDir(),"
                f"keys:Object.keys(env.parseEnvFile({json.dumps(str(dotenv))}))}}));")
        native = subprocess.run([str(bun), "-e", code], cwd=cwd, env=environment,
                                capture_output=True, text=True, timeout=15)
        (case / "native.log").write_text(native.stdout + native.stderr)
        require(native.returncode == 0, f"Native source oracle failed: {label}")
        observed = json.loads(native.stdout)
        require(observed["config"] == str(home / (".redirected" if redirected else ".omp")),
                f"Native dotenv oracle changed: {label}")
        for operation, result in (("discover", discovery), ("resolve", resolution), ("prepare", preparation)):
            if refuse:
                require(result.get("status") == "unavailable" and
                        result.get("reason") == "native_profile_environment_unsupported",
                        f"Unresolved dotenv source was admitted: {label}/{operation}")
            else:
                require(result["status"] == "success", f"Credential-only dotenv was refused: {label}/{operation}")
        if not refuse:
            require(preparation["payload"]["account_context"] == account,
                    f"Credential-only source acknowledgment changed: {label}")
        row = {"case": label, "native": observed, "discover": discovery,
               "resolve": resolution, "prepare": preparation}
        rows.append(row)
        print(json.dumps({"case": label, "boundary": "refused" if refuse else "preserved",
                          "native_redirected": redirected}), flush=True)
    (output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
    (output / "identity.json").write_text(json.dumps({"adapter_sha256": digest(package / "adapter/omp_adapter"),
        "bun_sha256": digest(bun), "native_dirs_sha256": digest(utils / "dirs.ts"),
        "native_env_sha256": digest(utils / "env.ts"), "probe_sha256": digest(Path(__file__)),
        "host_simulator_sha256": digest(simulator)}, indent=2) + "\n")
    print(json.dumps({"result": "passed", "cases": len(rows), "model_requests": 0}))


if __name__ == "__main__":
    main()
