"""Opt-in native OMP acceptance using a local model and task-control relay.

Run against an already installed official OMP package and an already built
add-on. No remote model, API key, or pre-existing agent configuration is used.
This is deliberately separate from the portable parser/SDK contract tests.
"""

import argparse
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import queue
import re
import shlex
import signal
import struct
import subprocess
import sys
import threading
import time

if os.name == "posix":
    import fcntl
    import pty
    import termios


ANSWER = "Native OMP completed the isolated fixture."
PROMPT = "Complete the isolated local fixture and report its result."
HOST_CONTEXT = "Include the isolated Logonomic host context in this native turn."
MODEL = "logonomic-fixture/model"
CHILD_MODEL = "logonomic-fixture/child"
CHILD_PROMPT = "NATIVE_CHILD_TASK: return the isolated child result."
GRANDCHILD_PROMPT = "NATIVE_GRANDCHILD_TASK: return the isolated grandchild result."
CHILD_ANSWER = "NATIVE_CHILD_RESULT"
GRANDCHILD_ANSWER = "NATIVE_GRANDCHILD_RESULT"
OBSERVATION_SCENARIOS = ("lifecycle", "child", "nested-child", "cwd")
BASE_ENVIRONMENT = {"PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
                    "SystemRoot", "TEMP", "TMP", "XDG_CONFIG_HOME", "XDG_DATA_HOME"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def chunks(data):
    return [base64.b64encode(data).decode()] if data else []


def unchunks(parts):
    return b"".join(base64.b64decode(part, validate=True) for part in parts)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf8")


def record_metadata(arguments):
    files = {"harness": Path(__file__), "driver": arguments.driver,
        "host_simulator": arguments.host_simulator}
    native = {}
    if arguments.omp is not None:
        files["native_cli"] = arguments.omp
    else:
        cli = arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent"
        bun = arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"
        files.update({"native_cli": cli / "dist/cli.js", "bun": bun})
        native["native_package"] = json.loads((cli / "package.json").read_text())["version"]
        native["bun"] = subprocess.check_output([str(bun), "--version"], text=True, timeout=5).strip()
    if arguments.host_driver is not None:
        files["host_driver"] = arguments.host_driver
    files.update({"package/" + str(path.relative_to(arguments.package)): path
        for path in arguments.package.rglob("*") if path.is_file()})
    write_json(arguments.artifacts / "metadata.json", {
        "platform": platform.platform(), "python": platform.python_version(),
        **native,
        "file_sha256": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in sorted(files.items())},
        "native_session_persistence": False,
        "model_transport": "isolated loopback OpenAI-compatible fixture",
    })


def relay_fixture():
    parser = argparse.ArgumentParser()
    parser.add_argument("--descriptor", type=Path, required=True)
    parser.add_argument("--server", required=True)
    arguments = parser.parse_args(sys.argv[2:])
    descriptor = json.loads(arguments.descriptor.read_text())
    require(arguments.server == descriptor["server_id"], "Native relay changed the host server ID")
    with Path(descriptor["log"]).open("a", encoding="utf8", buffering=1) as log:
        log.write(json.dumps({"argv": sys.argv[2:], "pid": os.getpid()}) + "\n")
        for line in sys.stdin:
            request = json.loads(line)
            log.write(json.dumps(request) + "\n")
            if "id" not in request:
                continue
            method = request["method"]
            if method == "initialize":
                result = {"protocolVersion": request["params"]["protocolVersion"],
                          "capabilities": {"tools": {}},
                          "serverInfo": {"name": "logonomic-native-fixture", "version": "1"}}
            elif method == "tools/list":
                result = {"tools": [{"name": name, "description": "Report the local fixture result.",
                                     "inputSchema": {"type": "object", "properties": {
                                         "message": {"type": "string"}}, "required": ["message"]}}
                                    for name in ("report_result", "send_message")]}
            elif method == "tools/call":
                require(request["params"]["name"] == "report_result", "Unexpected task action")
                require(request["params"]["arguments"]["message"] == "native task result",
                        "Native relay changed the task action payload")
                result = {"content": [{"type": "text", "text": "Host accepted the fixture result."}]}
            elif method == "ping":
                result = {}
            else:
                print(json.dumps({"jsonrpc": "2.0", "id": request["id"],
                                  "error": {"code": -32601, "message": "Unknown fixture method"}}), flush=True)
                continue
            print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)


def wrapper_fixture():
    descriptor = json.loads(Path(sys.argv[2]).read_text())
    arguments = sys.argv[3:]
    body = json.loads(sys.stdin.read()) if "--args-stdin" in arguments else None
    with Path(descriptor["log"]).open("a", encoding="utf8") as log:
        log.write(json.dumps({"argv": arguments, "body": body, "pid": os.getpid()}) + "\n")
    if descriptor.get("stall"):
        time.sleep(60)
        return
    if arguments[0] == "request-title-instruction":
        result = {"accepted": True, "instruction_required": True, "instruction": HOST_CONTEXT}
    else:
        result = {"accepted": True, "epoch": 1}
        if descriptor.get("children"):
            result["helper_response"] = {"child_state_version": 1}
    print(json.dumps(result), flush=True)


class Local_model:
    def __init__(self, scenario):
        self.requests = []
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_arguments):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                fixture.requests.append({"path": self.path, "body": body})
                require(self.path == "/v1/chat/completions", "Native model used an unexpected endpoint")
                if scenario.startswith("fallback-") and body["model"] == "model":
                    self.send_response(429)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Retry-After", "0.001")
                    self.end_headers()
                    self.wfile.write(json.dumps({"error": {"message": "fixture rate limit",
                        "type": "rate_limit_error"}}).encode())
                    return
                tools = [tool["function"]["name"] for tool in body.get("tools", [])]
                tool_results = [message for message in body["messages"] if message.get("role") == "tool"]
                tool_call = None
                child = grandchild = False
                if scenario in ("child", "nested-child"):
                    user_text = json.dumps([message.get("content") for message in body["messages"]
                                            if message.get("role") == "user"])
                    root = body["model"] == "model" and PROMPT in user_text
                    grandchild = not root and GRANDCHILD_PROMPT in user_text
                    child = not root and not grandchild and CHILD_PROMPT in user_text
                    require(root or child or grandchild, "Native Task lost its exact assignment")
                    # OMP also asks its small model to label a Task assignment.
                    # That request has no tools and is not a child agent turn.
                    should_spawn = bool(tools) and (root or (child and scenario == "nested-child"))
                    if should_spawn and not tool_results:
                        require("task" in tools, "The real native Task tool is unavailable")
                        assignment = GRANDCHILD_PROMPT if child else CHILD_PROMPT
                        item = {"name": "GrandchildFixture" if child else "ChildFixture", "task": assignment,
                            "solutionSpace": "One fixed local-fixture reply; no file changes."}
                        schema = next(tool["function"]["parameters"] for tool in body["tools"]
                                      if tool["function"]["name"] == "task")
                        task_arguments = {"context": "Isolated local native-child acceptance.", "tasks": [item]} \
                            if "tasks" in schema["properties"] else item
                        if "i" in schema["properties"]:
                            task_arguments["i"] = "Run the isolated native child"
                        tool_call = ("task", task_arguments)
                elif scenario == "delegated" and "Host accepted the fixture result." not in json.dumps(tool_results):
                    require(len(fixture.requests) <= 3, "Native task action did not reach its relay")
                    name = next((name for name in tools if name.endswith("_report_result")), None)
                    if name:
                        tool_call = (name, {"message": "native task result"})
                    else:
                        routes = re.findall(r"xd://([a-z0-9_]+_report_result)\b", json.dumps(tool_results))
                        if routes:
                            require("write" in tools, "Native xdev dispatch tool is unavailable")
                            tool_call = ("write", {"path": "xd://" + routes[0],
                                "content": json.dumps({"message": "native task result"})})
                        else:
                            require("read" in tools, "Native xdev inventory tool is unavailable")
                            tool_call = ("read", {"path": "xd://"})
                elif scenario in ("background", "persistent") and not tool_results:
                    require("bash" in tools, "Native asynchronous shell tool is unavailable")
                    seconds = 30 if scenario == "persistent" else 3
                    tool_call = ("bash", {"command": f"sleep {seconds}", "timeout": seconds + 2, "async": True})
                if tool_call:
                    name, tool_arguments = tool_call
                    delta = {"role": "assistant", "tool_calls": [{"index": 0, "id": f"fixture-call-{len(fixture.requests)}",
                        "type": "function", "function": {"name": name,
                            "arguments": json.dumps(tool_arguments)}}]}
                    finish = "tool_calls"
                else:
                    content = GRANDCHILD_ANSWER if grandchild else CHILD_ANSWER if child else \
                        "Native work is still pending." if scenario in ("background", "persistent") and len(fixture.requests) == 2 else ANSWER
                    delta, finish = {"role": "assistant", "content": content}, "stop"
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                for content, reason in ((delta, None), ({}, finish)):
                    frame = {"id": "fixture-completion", "object": "chat.completion.chunk", "created": 1,
                        "model": "model", "choices": [{"index": 0, "delta": content, "finish_reason": reason}]}
                    self.wfile.write(("data: " + json.dumps(frame) + "\n\n").encode())
                    self.wfile.flush()
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class Native_case:
    def __init__(self, arguments, name):
        self.arguments = arguments
        self.name = name
        self.root = arguments.artifacts / name
        self.root.mkdir(parents=True, exist_ok=False)
        for part in ("bin", "home", "agent", "workspace", "natives", "plugin"):
            (self.root / part).mkdir()
        if arguments.omp is None:
            bun = arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"
            cli = arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent/dist/cli.js"
            require(bun.is_file() and cli.is_file(), "Install the official OMP and Bun packages before this gate")
            launcher = self.root / "bin/omp"
            launcher.write_text(f"#!/bin/sh\nexec {shlex.quote(str(bun))} {shlex.quote(str(cli))} \"$@\"\n")
            launcher.chmod(0o755)
        else:
            launcher = arguments.omp
        system_path = str(Path(os.environ["SystemRoot"]) / "System32") if os.name == "nt" else "/usr/bin:/bin"
        self.environment = {"PATH": os.pathsep.join((str(launcher.parent), system_path)), "LANG": "C.UTF-8",
            "HOME": str(self.root / "home"), "PI_CODING_AGENT_DIR": str(self.root / "agent"),
            "PI_NATIVES_DIR": str(self.root / "natives"), "PI_NO_TITLE": "1", "PI_NO_PTY": "1"}
        if os.name == "nt":
            self.environment.update({"USERPROFILE": str(self.root / "home"), "PATHEXT": ".EXE",
                "SystemRoot": os.environ["SystemRoot"], "TEMP": str(self.root), "TMP": str(self.root),
                "APPDATA": str(self.root / "home/Roaming"), "LOCALAPPDATA": str(self.root / "home/Local")})
        agent_directory = self.root / "agent"
        if name == "profile":
            self.environment.update({"OMP_PROFILE": "chosen", "PI_PROFILE": "ignored", "PI_CONFIG_DIR": ".omp-fixture"})
            agent_directory = self.root / "home/.omp-fixture/profiles/chosen/agent"
            agent_directory.mkdir(parents=True)
        self.model = Local_model(name)
        write_json(agent_directory / "models.yml", {"providers": {"logonomic-fixture": {
            "baseUrl": f"http://127.0.0.1:{self.model.server.server_port}/v1", "auth": "none",
            "api": "openai-completions", "models": [{"id": model, "name": "Local fixture",
                "reasoning": False, "input": ["text"], "cost": {"input": 0, "output": 0,
                "cacheRead": 0, "cacheWrite": 0}, "contextWindow": 32768, "maxTokens": 1024}
                for model in (("model", "fallback") if name.startswith("fallback-") else
                              ("model", "alternate") if name == "idle-model" else
                              ("model", "child") if name in ("child", "nested-child") else ("model",))]}}})
        if name == "idle-model":
            models = json.loads((agent_directory / "models.yml").read_text())
            models["providers"]["logonomic-fixture"]["models"][1]["reasoning"] = True
            write_json(agent_directory / "models.yml", models)
        settings = {"enabledProviders": ["logonomic-fixture"],
            "telemetry": {"otlpExportEnabled": False}, "marketplace": {"autoUpdate": "off"},
            "power": {"sleepPrevention": "off"}}
        if name in ("initial-prompt", "wake"):
            # The isolated local provider is already configured. The ordinary
            # native setup wizard otherwise waits for a human account choice
            # before main.ts dispatches startup messages; no setup-version or
            # account state is fabricated and production preparation sets none.
            settings["startup"] = {"setupWizard": False, "checkUpdate": False, "quiet": True}
        if name in ("child", "nested-child"):
            settings["modelRoles"] = {"task": CHILD_MODEL, "smol": MODEL}
        if name == "compaction":
            settings["modelRoles"] = {"smol": MODEL}
            settings["compaction"] = {"keepRecentTokens": 1, "methodOrder": ["soft"]}
        if name in ("background", "persistent"):
            settings["async"] = {"enabled": True}
        if name.startswith("fallback-"):
            settings["retry"] = {"enabled": True, "maxRetries": 0, "baseDelayMs": 1, "maxDelayMs": 50,
                "modelFallback": True, "rateLimitWalksChain": True,
                "fallbackChains": {MODEL: ["logonomic-fixture/fallback"]}}
        write_json(agent_directory / "config.yml", settings)
        self.argv = ["omp", "--no-session", "--model", MODEL]
        self.expected_cwd = self.root / "workspace"
        if name == "cwd":
            self.expected_cwd = self.root / "native-cwd"
            self.expected_cwd.mkdir()
            self.argv.extend(["--cwd", str(self.expected_cwd)])
        if name in ("child", "nested-child", "initial-prompt", "wake"):
            self.add_native_observer()
        self.transcript = []

    def add_native_observer(self):
        # This diagnostic extension observes public native messages/buses; it
        # never synthesizes callbacks, dispatches work or changes session state.
        observer = self.root / "native-observer.ts"
        journal = json.dumps(str(self.root / "raw-native-events.jsonl"))
        observer.write_text('import { appendFileSync } from "node:fs";\n' +
            'export default function(pi) {\n' +
            ' const save = (event, value) => appendFileSync(' + journal + ', JSON.stringify({event, value}) + "\\n");\n' +
            ' pi.on("session_start", (_event, ctx) => save("session_start", {agent:ctx.agent, session:ctx.sessionManager.getSessionId()}));\n' +
            ' pi.on("agent_start", (_event, ctx) => save("agent_start", {agent:ctx.agent, session:ctx.sessionManager.getSessionId(), pid:process.pid}));\n' +
            ' pi.on("message_start", (event, ctx) => { if(event.message.role === "user") save("user_message", {agent:ctx.agent, session:ctx.sessionManager.getSessionId(), message:event.message}); });\n' +
            ' for (const name of ["task:subagent:lifecycle", "task:subagent:progress", "task:subagent:event"]) pi.events.on(name, value => save(name, value));\n' +
            '}\n', encoding="utf8")
        self.argv.extend(["--extension", str(observer)])

    def helper_environment(self, name="wrapper", stall=False):
        descriptor = self.root / f"{name}-descriptor.json"
        write_json(descriptor, {"log": str(self.root / f"{name}.jsonl"), "stall": stall,
                               "children": self.name in ("child", "nested-child")})
        wrapper = self.root / "bin" / name
        wrapper.write_text(f"#!/bin/sh\nexec {shlex.quote(sys.executable)} "
            f"{shlex.quote(str(Path(__file__).resolve()))} --wrapper-fixture "
            f"{shlex.quote(str(descriptor))} \"$@\"\n")
        wrapper.chmod(0o755)
        return {"LOGONOMIC_VNMCTL_PATH": str(wrapper), "LOGONOMIC_SESSION_CAPABILITY": "a" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.omp",
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "b" * 64,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": "c" * 32,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(self.arguments.package / "helpers")}

    def check_helper_custody(self):
        for close_input in (True, False):
            name = "cancel-wrapper" if close_input else "deadline-wrapper"
            environment = dict(self.environment, **self.helper_environment(name, stall=True))
            helper = subprocess.Popen([str(self.arguments.package / "helpers/omp_helper")],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
            started = time.monotonic()
            wrapper_pid = None
            try:
                request = {"operation": "event", "name": "session_start", "body": {
                    "session_id": "custody-fixture", "activation_nonce": "d" * 32}}
                helper.stdin.write((json.dumps(request) + "\n").encode())
                helper.stdin.flush()
                journal = self.root / f"{name}.jsonl"
                while (not journal.exists() or journal.stat().st_size == 0) and time.monotonic() - started < 0.6:
                    time.sleep(0.01)
                require(journal.exists(), "The helper never started the custody fixture wrapper")
                wrapper_pid = json.loads(journal.read_text().splitlines()[0])["pid"]
                if close_input:
                    helper.stdin.close()
                helper.wait(timeout=2)
                require(time.monotonic() - started < 1.5, "Helper cancellation exceeded its bounded deadline")
                try:
                    os.kill(wrapper_pid, 0)
                except ProcessLookupError:
                    pass
                else:
                    raise RuntimeError("The helper exited before reaping its wrapper child")
            finally:
                if helper.poll() is None:
                    helper.kill()
                    helper.wait()
                if wrapper_pid is not None:
                    try:
                        os.kill(wrapper_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def frozen_environment(self, capability, version):
        sidecar = self.arguments.package / "capabilities" / f"{capability}@{version}.json"
        declaration = json.loads(sidecar.read_text())["payload"]
        allowed = BASE_ENVIRONMENT | set(declaration.get("environment_names", []))
        return {name: value for name, value in self.environment.items() if name in allowed}

    def prepare_request(self, delegated):
        request = {"purpose": "implementation", "launcher": {"script_body": " ".join(self.argv),
            "runner_key": "direct", "environment_text": ""}, "launcher_argv": self.argv,
            "working_directory": str(self.root / "workspace"),
            "launch_environment": {"platform": self.arguments.platform_id, "effective_environment":
                self.frozen_environment("delegated-session" if delegated else "structured-session", 3)},
            "prompt_base64": base64.b64encode(PROMPT.encode()).decode(), "declared_assets": []}
        if delegated:
            relay = self.root / "bin/fixture-relay"
            relay.write_text(f"#!/bin/sh\nexec {shlex.quote(sys.executable)} "
                f"{shlex.quote(str(Path(__file__).resolve()))} --relay-fixture \"$@\"\n")
            relay.chmod(0o755)
            descriptor = self.root / "relay-descriptor.json"
            write_json(descriptor, {"server_id": "task-control.alpha", "log": str(self.root / "relay.jsonl")})
            request["control"] = {"version": 1, "task_id": "native-fixture", "servers": [
                {"server_id": "task-control.alpha", "program": "/must-not-launch-original-server",
                 "actions": ["report_result", "send_message"], "environment": {}}],
                "private_artifact_root": str(self.root / "plugin"),
                "connection": {"version": 1, "relay_program": str(relay), "descriptor_path": str(descriptor)}}
            if self.name != "fallback-preserved":
                request["model_selection"] = {"version": 1, "model": MODEL}
        return request

    def host(self, capability, version, operation, payload, name):
        if self.name == "wake" and (capability == "session-wake" or
                (capability == "launch-preparation" and "session_wake" in payload)):
            # The SDK CLI transcript intentionally redacts all private wake
            # bindings, including the environment path needed for launch. Its
            # public typed-result API preserves the genuine adapter response.
            require(self.arguments.host_driver is not None,
                    "Native wake requires --host-driver pointing at omp_native_host_driver")
            result = subprocess.run([str(self.arguments.host_driver), str(self.arguments.package)],
                input=json.dumps({"capability_id": capability, "version": version,
                    "operation": operation, "payload": payload}), capture_output=True, text=True, timeout=20)
            require(result.returncode == 0, f"SDK typed native host failed: {result.stderr}")
            outcome = json.loads(result.stdout)
            require(outcome.get("status") == "success", f"Actual packaged adapter refused native {name}")
            # Private binding bytes remain in this process and the native
            # registration. Evidence records only the invocation's status.
            write_json(self.root / f"host-{name}.json", {"capability_id": capability,
                "version": version, "operation": operation, "status": outcome["status"]})
            return outcome["payload"]
        script = {"script_version": 1, "steps": [{"op": "invoke", "id": "request",
            "capability_id": capability, "version": version, "operation": operation, "payload": payload},
            {"op": "wait_result", "id": "request", "timeout_ms": 8000}, {"op": "shutdown"}]}
        path = self.root / f"host-{name}.json"
        write_json(path, script)
        result = subprocess.run([str(self.arguments.host_simulator), str(self.arguments.package),
            "--script", str(path), "--platform", self.arguments.platform_id], capture_output=True, text=True, timeout=15)
        (self.root / f"host-{name}.txt").write_text(result.stdout + result.stderr)
        require(result.returncode == 0, f"SDK host simulator failed: {result.stderr}")
        frames = [json.loads(line[2:]) for line in result.stdout.splitlines() if line.startswith("< ")]
        terminal = next((frame for frame in frames if frame.get("type") == "result"), None)
        require(terminal is not None and terminal.get("status") == "success", f"Adapter rejected {name}: {terminal}")
        return terminal["payload"]

    def inspect(self, state, output=b"", error=b"", final=False, exit_code=0, timed_out=False):
        request = {"purpose": "implementation", "failure_diagnostics": {"version": 1}, "model_state": {"version": 1},
            "inspection": {"protocol_state_base64_chunks": state,
            "stdout_base64_chunks": chunks(output), "stderr_base64_chunks": chunks(error),
            "final": final, "timed_out": timed_out, "exit_code": exit_code}}
        result = subprocess.run([str(self.arguments.driver), "--inspect"], input=json.dumps(request),
            text=True, capture_output=True, timeout=5)
        require(result.returncode == 0, f"Candidate inspection driver failed: {result.stderr}")
        response = json.loads(result.stdout)
        self.transcript.append({"inspection": request["inspection"], "result": response})
        return response, request

    def prepare_interactive(self, initial_prompt=None, wake=False):
        request = self.prepare_request(False)
        interactive = {key: value for key, value in request.items() if key not in ("purpose", "prompt_base64")}
        interactive["declared_assets"] = json.loads((self.arguments.package /
            "capabilities/managed-assets@1.json").read_text())["payload"]["assets"]
        interactive["title_instruction"] = HOST_CONTEXT
        interactive["launch_environment"]["effective_environment"] = self.frozen_environment("launch-preparation", 3)
        if initial_prompt is not None:
            interactive["initial_prompt"] = initial_prompt
        if wake:
            profile = self.root / "host-profile"
            profile.mkdir(mode=0o700)
            interactive["profile_storage"] = {"version": 1, "root": str(profile)}
            interactive["session_wake"] = {"version": 1}
        patch = self.host("launch-preparation", 3, "prepare", interactive, "interactive")
        require(patch.get("disposition", {}).get("lifecycle_events") is True,
                "Native interactive launcher did not adopt packaged lifecycle hooks")
        injected = [argument.replace("{{asset:assets/logonomic.ts}}",
            str(self.arguments.package / "assets/logonomic.ts")) for argument in patch["argv"]]
        environment = dict(self.frozen_environment("launch-preparation", 3), **self.helper_environment())
        environment.update(patch.get("environment", {}).get("set", {}))
        for name in patch.get("environment", {}).get("unset", []):
            environment.pop(name, None)
        return patch, injected, environment

    def delivered_events(self):
        path = self.root / "wrapper.jsonl"
        if not path.exists():
            return []
        journal = [json.loads(line) for line in path.read_text().splitlines()]
        return [row["body"]["event"] for row in journal if row["argv"][:2] == ["provider-event", "deliver"]]

    def check_observations(self):
        events = self.delivered_events()
        write_json(self.root / "native-observations.json", events)
        require(events and events[0]["name"] == "session_start" and events[-1]["name"] == "session_shutdown",
                "Native observation lifetime does not match its root session")
        require(not any(event["name"] == "logonomic_observation_lost" for event in events),
                "Native lifecycle observation degraded during the fixture")
        start = events[0]["body"]
        ledger = {"current_epoch": 0, "current_transition": None}
        classifications = []
        for index, event in enumerate(events):
            request = {
                "event": event, "ledger": ledger, "transition_nonce": event["body"]["activation_nonce"],
                "context_state_version": 2, "execution_context": {"version": 1}, "model_state": {"version": 1}}
            if self.name in ("child", "nested-child"):
                request["child_state"] = {"version": 1}
            classification = self.host("helper-ingress", 2, "classify_event", request, f"native-event-{index}")
            classifications.append(classification)
            ledger = {"current_epoch": 1, "current_transition": {"kind": "start",
                "provider_session_id": start["session_id"], "nonce": start["activation_nonce"]}}
        write_json(self.root / "native-classifications.json", classifications)
        observations = [row for result in classifications for row in result.get("observations", [])]
        require(any(row.get("outcome_transition") == "succeeded" for row in observations),
                "Actual native completion never became a host lifecycle success")
        require(not any(result.get("context_failure") or result.get("execution_context", {}).get("failure") or
                        result.get("model_state", {}).get("failure") for result in classifications),
                "Actual native optional telemetry was refused by the adapter")
        context = [row for result in classifications for row in result.get("context_updates", [])]
        execution = [row for result in classifications for row in result.get("execution_context", {}).get("updates", [])]
        models = [row for result in classifications for row in result.get("model_state", {}).get("updates", [])]
        require(any(row.get("used_tokens", -1) >= 0 and row.get("context_window") == 32768 for row in context),
                "Native current-context measurement never reached the host contract")
        require(execution and all(row["state"] == "confirmed" and row["cwd"] == str(self.expected_cwd)
                                  and row["environment"] == "local" for row in execution),
                "Native working directory was not preserved in host observations")
        root_models = [row for row in models if not row["agent_id"]]
        require(root_models and any(row["state"] == "confirmed" and row["provider"] == "logonomic-fixture" and
                                    row["model"] == "model" for row in root_models),
                "Actual root model/provider never reached the host contract")
        starts = [row["agent_id"] for row in observations if row.get("subagent_transition") == "started"]
        stops = [row["agent_id"] for row in observations if row.get("subagent_transition") == "stopped"]
        if self.name in ("child", "nested-child"):
            raw = [json.loads(line) for line in (self.root / "raw-native-events.jsonl").read_text().splitlines()]
            require(any(row["event"] == "task:subagent:lifecycle" and row["value"].get("status") == "started" for row in raw),
                    "Native child never started; inspect raw events/tool results for native session-lock or other startup refusal")
            expected = 2 if self.name == "nested-child" else 1
            require(len(starts) == expected and len(set(starts)) == expected and sorted(stops) == sorted(starts),
                    "Actual native descendants did not retain their exact start/stop identities")
            child_updates = [row for result in classifications for row in result.get("child_state", {}).get("updates", [])]
            direct = {row["agent_id"] for row in child_updates if not row["parent_agent_id"]}
            require(len(direct) == 1 and direct.issubset(starts) and
                    all(any(row["agent_id"] == child and row["state"] == "succeeded" for row in child_updates) for child in starts),
                    "Actual native child outcomes/parent identities never reached the child-state contract")
            require(any(row["agent_id"] in direct and row["state"] == "confirmed" and
                        row["provider"] == "logonomic-fixture" and row["model"] == "child" for row in models),
                    "Actual serving child model/provider never reached the host contract")
            child_turns = [request for request in self.model.requests if request["body"].get("tools") and
                           request["body"]["model"] == "child" and
                           CHILD_PROMPT in json.dumps([message.get("content") for message in
                               request["body"]["messages"] if message.get("role") == "user"])]
            native_messages = json.dumps(self.model.requests)
            require(child_turns and CHILD_ANSWER in native_messages,
                    "The native child did not make and return its own real local model turn")
            if self.name == "nested-child":
                require(any(row["parent_agent_id"] in direct and row.get("depth") == 2 for row in child_updates),
                        "Actual nested child did not retain its structural parent/depth")
                require(GRANDCHILD_PROMPT in native_messages and GRANDCHILD_ANSWER in native_messages,
                        "The real native grandchild did not execute")
        return {"observed_events": len(events), "direct_children": len(starts), "model_updates": len(models),
                "context_updates": len(context), "execution_updates": len(execution)}

    def run_initial_prompt(self):
        process = None
        master = slave = None
        output = []
        try:
            brief = self.root / "continuation brief.md"
            brief.write_text("The isolated host continuation brief.\n", encoding="utf8")
            text = f"Logonomic continuation: read {brief}; preserve café, 'quotes' and $() literally."
            self.argv.append("--no-title")
            wake = self.name == "wake"
            patch, command, environment = self.prepare_interactive(
                {"version": 1, "text": text, "referenced_file": str(brief)}, wake=wake)
            require(patch.get("initial_prompt", {}).get("text_sha256") == hashlib.sha256(text.encode()).hexdigest(),
                    "Actual adapter did not acknowledge the exact initial prompt bytes")
            environment["TERM"] = "xterm-256color"
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 120, 0, 0))
            process = subprocess.Popen(command, cwd=self.root / "workspace", env=environment,
                stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
            os.close(slave)
            slave = None

            def read_terminal():
                try:
                    while data := os.read(master, 65536):
                        output.append(data)
                except OSError:
                    pass

            reader = threading.Thread(target=read_terminal, daemon=True)
            reader.start()
            deadline = time.monotonic() + 45
            while not any(row["name"] == "agent_end" for row in self.delivered_events()):
                require(process.poll() is None and time.monotonic() < deadline,
                        "Native interactive initial prompt did not reach its terminal event")
                time.sleep(0.02)
            raw = [json.loads(line) for line in (self.root / "raw-native-events.jsonl").read_text().splitlines()]
            users = [row["value"]["message"] for row in raw if row["event"] == "user_message" and
                     row["value"].get("agent", {}).get("kind") == "main"]
            require(len(users) == 1 and users[0]["content"] == [{"type": "text", "text": text}],
                    "The native interactive session did not receive exactly the requested first user message")
            require(len(self.model.requests) == 1 and text in json.dumps(self.model.requests, ensure_ascii=False),
                    "The exact native startup prompt never reached its actual model turn")
            if wake:
                offer = patch.get("session_wake", {})
                require(offer.get("available") is True and "binding" in offer,
                        "Actual preparation did not bind an available native wake endpoint")
                start = next(row["body"] for row in self.delivered_events() if row["name"] == "session_start")
                target = {"session": {"provider_session_id": start["session_id"],
                    "working_directory": str(self.root / "workspace"), "launcher_argv": self.argv,
                    "launch_environment": {"platform": self.arguments.platform_id,
                        "effective_environment": self.frozen_environment("session-wake", 1)}},
                    "correlation": {"version": 1, "transition_nonce": start["activation_nonce"]},
                    "binding": offer["binding"]}
                ready = False
                for attempt in range(20):
                    inspected = self.host("session-wake", 1, "inspect", target, f"wake-inspect-{attempt}")
                    if inspected.get("available") is True:
                        ready = True
                        break
                    require(inspected.get("reason") in ("unbound_session", "native_busy"),
                            f"Native wake inspection refused: {inspected}")
                    time.sleep(0.05)
                require(ready and len(self.model.requests) == 1, "Read-only native wake inspection never established idle readiness")
                instruction = "Logonomic: consume the newly committed local fixture mailbox and report completion."
                notified = self.host("session-wake", 1, "notify", {**target,
                    "notification_id": "native-mailbox-fixture", "instruction": instruction}, "wake-notify")
                require(notified == {"disposition": "uncertain", "reason": "native_admission_unacknowledged"},
                        "Void native aside submission must not overstate synchronous delivery acknowledgment")
                while sum(row["name"] == "agent_end" for row in self.delivered_events()) < 2:
                    require(process.poll() is None and time.monotonic() < deadline,
                            "Actual native wake did not produce a second terminal turn")
                    time.sleep(0.02)
                require(len(self.model.requests) == 2 and instruction in json.dumps(self.model.requests[-1]),
                        "The wake instruction did not reach a second real model request")
                raw = [json.loads(line) for line in (self.root / "raw-native-events.jsonl").read_text().splitlines()]
                turns = [row["value"] for row in raw if row["event"] == "agent_start" and
                         row["value"].get("agent", {}).get("kind") == "main"]
                require(len(turns) == 2 and all(row["session"] == start["session_id"] and row["pid"] == process.pid for row in turns),
                        "Native wake must reuse the exact process and session")
                require(sum(row["event"] == "user_message" for row in raw) == 1 and
                        sum(row["name"] == "session_start" for row in self.delivered_events()) == 1,
                        "Native wake cannot restart the session or submit a simulated editor/user prompt")
            # The composer is idle and empty; native Ctrl+D owns orderly exit.
            os.write(master, b"\x04")
            exit_code = process.wait(timeout=8)
            reader.join(timeout=2)
            require(exit_code == 0, f"Native interactive shutdown exited {exit_code}")
            observed = self.check_observations()
            return {"scenario": self.name, "exit_code": exit_code, "model_requests": len(self.model.requests),
                    "exact_first_user_message": True, **({"same_process_native_wake": True} if wake else {}), **observed}
        finally:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            for descriptor in (master, slave):
                if descriptor is not None:
                    os.close(descriptor)
            (self.root / "terminal.bin").write_bytes(b"".join(output))
            write_json(self.root / "model-requests.json", self.model.requests)
            self.model.close()

    def run_native_control(self):
        # Exercise native public RPC selectors/compaction while keeping the
        # same extension-loaded process alive. This is an observation gate,
        # separate from the add-on's one-task structured-session inspector.
        process = None
        frames = []
        errors = []
        try:
            _patch, command, environment = self.prepare_interactive()
            command.extend(["--mode", "rpc", "--no-ui"])
            process = subprocess.Popen(command, cwd=self.root / "workspace", env=environment,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            incoming = queue.Queue()

            def read(stream, name):
                for line in iter(stream.readline, b""):
                    incoming.put((name, line))
                incoming.put((name, None))

            for stream, name in ((process.stdout, "stdout"), (process.stderr, "stderr")):
                threading.Thread(target=read, args=(stream, name), daemon=True).start()
            deadline = time.monotonic() + 45

            def until(predicate):
                while time.monotonic() < deadline:
                    name, data = incoming.get(timeout=max(0.01, deadline - time.monotonic()))
                    require(data is not None, "Native process closed before the requested observation")
                    if name == "stderr":
                        errors.append(data.decode(errors="replace"))
                        continue
                    frame = json.loads(data)
                    frames.append(frame)
                    if predicate(frame):
                        return frame
                raise RuntimeError("Native control scenario exceeded its deadline")

            def rpc(identifier, command_type, **arguments):
                command = {"id": identifier, "type": command_type, **arguments}
                process.stdin.write((json.dumps(command) + "\n").encode())
                process.stdin.flush()
                result = until(lambda row: row.get("type") == "response" and row.get("id") == identifier)
                require(result.get("success") is True, f"Native {command_type} refused: {result}")
                return result.get("data")

            def prompt(identifier, text):
                rpc(identifier, "prompt", message=text)
                result = until(lambda row: row.get("type") == "prompt_result" and row.get("id") == identifier)
                require(result.get("status") == "completed", "Native observation fixture turn failed")
                if result.get("sessionSettled") is not True:
                    until(lambda row: row.get("type") == "session_settled")

            until(lambda row: row.get("type") == "ready")
            rpc("protocol", "negotiate_protocol", protocolVersion=2)
            rpc("filter", "set_event_filter", events=None, messageUpdates="delta")
            prompt("first", PROMPT)
            if self.name == "idle-model":
                baseline_requests = len(self.model.requests)
                rpc("select", "set_model", provider="logonomic-fixture", modelId="alternate")
                rpc("reasoning", "set_thinking_level", level="low")
                state = rpc("state", "get_state")
                require(state["model"]["id"] == "alternate" and state["thinkingLevel"] == "low" and
                        state["isSettled"] is True, "Native idle model/thinking selection was not actually applied")
                observed_deadline = time.monotonic() + 5
                while not any(row["name"] == "model_observation" and
                        row["body"].get("model", {}).get("model") == "alternate" and
                        row["body"].get("model", {}).get("reasoning_effort") == "low" for row in self.delivered_events()):
                    require(time.monotonic() < observed_deadline, "Idle native model change was not reported")
                    time.sleep(0.02)
                require(len(self.model.requests) == baseline_requests == 1,
                        "Observing an idle model change must not start another model request")
            elif self.name == "compaction":
                prompt("second", "Second isolated turn for the native compaction fixture.")
                rpc("compact", "compact", customInstructions="Preserve the two isolated fixture requests.")
                entries = rpc("entries", "get_entries")["entries"]
                compactions = [row for row in entries if row.get("type") == "compaction"]
                require(compactions, "Native compaction did not install a real history entry")
                observed_deadline = time.monotonic() + 5
                while not any(row["name"] == "session_compact" and
                        row["body"].get("context", {}).get("compaction_id") == compactions[-1]["id"]
                        for row in self.delivered_events()):
                    require(time.monotonic() < observed_deadline, "Installed native compaction was not reported")
                    time.sleep(0.02)
            process.stdin.close()
            exit_code = process.wait(timeout=8)
            require(exit_code == 0, f"Native observation process exited {exit_code}")
            observed = self.check_observations()
            classified = json.loads((self.root / "native-classifications.json").read_text())
            if self.name == "idle-model":
                require(any(row.get("model") == "alternate" and row.get("reasoning_effort") == "low"
                    for result in classified for row in result.get("model_state", {}).get("updates", [])),
                    "The actual adapter failed to report the idle native model/thinking selection")
            elif self.name == "compaction":
                require(any(row.get("compacted") is True and row.get("event_id") == "compaction:" + compactions[-1]["id"]
                    for result in classified for row in result.get("context_updates", [])),
                    "The actual adapter failed to report the installed native compaction")
            return {"scenario": self.name, "exit_code": exit_code, "model_requests": len(self.model.requests), **observed}
        finally:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            write_json(self.root / "native-frames.json", frames)
            (self.root / "native-stderr.txt").write_text("".join(errors))
            write_json(self.root / "model-requests.json", self.model.requests)
            self.model.close()

    def run(self, delegated):
        process = None
        try:
            discovered = self.host("launcher-discovery", 1, "discover", {"platform": self.arguments.platform_id,
                "ambient_environment": self.frozen_environment("launcher-discovery", 1),
                "default_home_fallback": str(self.root / "home")}, "discover")
            require(any(row.get("script_body") == "omp" for row in discovered["candidates"]),
                    "Actual adapter did not discover the installed native launcher")
            capability = "delegated-session" if delegated else "structured-session"
            request = self.prepare_request(delegated)
            if self.name == "profile":
                account_request = {"launcher": request["launcher"],
                    "launch_environment": {"platform": self.arguments.platform_id,
                        "effective_environment": self.frozen_environment("accounts", 2)},
                    "normalization_directory": str(self.root / "workspace"),
                    "default_home_fallback": str(self.root / "home")}
                accounts = self.host("accounts", 2, "discover", account_request, "account-discover")["accounts"]
                selected = self.host("accounts", 2, "resolve", account_request, "account-resolve")["adapter_account_key"]
                account = next((row for row in accounts if row["adapter_account_key"] == selected), None)
                require(account is not None and account["profile_path"] == str(self.root / "home/.omp-fixture/profiles/chosen")
                    and account["display_name"] == "chosen", "Account discovery did not resolve the exact native source profile")
                request["account_context"] = {"version": 2, "adapter_account_key": selected,
                    "profile_path": account["profile_path"]}
            if self.name in OBSERVATION_SCENARIOS:
                _patch, self.argv, _environment = self.prepare_interactive()
                request = self.prepare_request(False)
            prepared = self.host(capability, 3, "prepare", request, "prepare")
            if self.name == "profile":
                require(prepared.get("account_context") == request["account_context"],
                        "Native preparation did not acknowledge the exact selected source account")
            write_json(self.root / "prepared.json", prepared)
            for artifact in prepared.get("private_artifacts", []):
                target = self.root / "plugin" / artifact["relative_path"]
                require(target.resolve().is_relative_to((self.root / "plugin").resolve()),
                        "Adapter private artifact escaped its host-supplied root")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(base64.b64decode(artifact["content_base64"], validate=True))
            proposal = prepared["process"]
            environment = {name: self.environment[name] for name in proposal["environment_names"]}
            environment.update(proposal["environment"])
            if self.name in OBSERVATION_SCENARIOS:
                environment.update(self.helper_environment())
            command = [str(self.arguments.package / proposal["executable"]), *proposal["argv"]]
            process = subprocess.Popen(command, cwd=self.root / "workspace", env=environment,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=os.name == "posix")
            events = queue.Queue()

            def read(stream, name):
                for line in iter(stream.readline, b""):
                    events.put((name, line))
                events.put((name, None))

            for stream, name in ((process.stdout, "stdout"), (process.stderr, "stderr")):
                threading.Thread(target=read, args=(stream, name), daemon=True).start()
            state = prepared["inspection"]["protocol_state_base64_chunks"]
            deadline = time.monotonic() + 45
            eof = set()
            native_frames = []
            checked_serving_model = False
            selected_failure = None
            failure_request = None
            persistent_stopping = False
            while len(eof) < 2:
                require(time.monotonic() < deadline, "Native process exceeded its acceptance deadline")
                name, data = events.get(timeout=max(0.01, deadline - time.monotonic()))
                if data is None:
                    eof.add(name)
                    continue
                if name == "stdout":
                    frame = json.loads(data)
                    native_frames.append(frame)
                    write_json(self.root / "native-frames.json", native_frames)
                if selected_failure or persistent_stopping:
                    continue
                inspected, inspection_request = self.inspect(state, data if name == "stdout" else b"",
                                                              data if name == "stderr" else b"")
                if (self.name == "structured" and name == "stdout" and frame.get("type") == "message_end"
                        and frame.get("message", {}).get("role") == "assistant"):
                    model = inspected.get("model_state", {}).get("observation", {})
                    require(model.get("provider") == frame["message"]["provider"] and
                            model.get("model") == frame["message"]["model"],
                            "Structured inspection lost the serving model in the native assistant message")
                    require(self.host("structured-session", 3, "inspect", inspection_request, "serving-model") == inspected,
                            "Packaged adapter did not return the native serving model")
                    checked_serving_model = True
                if self.name == "fallback-selected" and inspected.get("failure_reason"):
                    require(inspected["failure_reason"] == "omp_native_error",
                            f"Expected the selected model's native refusal, got {inspected}")
                    selected_failure, failure_request = inspected, inspection_request
                    process.stdin.close()
                    continue
                require(not inspected.get("failure_reason"), f"Candidate rejected actual native output: {inspected}")
                require(not inspected["complete"], "Candidate completed before native process exit")
                state = inspected["protocol_state_base64_chunks"]
                pending = unchunks(inspected.get("stdin_base64_chunks", []))
                if pending:
                    process.stdin.write(pending)
                    process.stdin.flush()
                if inspected.get("close_stdin"):
                    process.stdin.close()
                if self.name == "persistent" and name == "stdout" and frame.get("type") == "prompt_result":
                    require(frame.get("sessionSettled") is False and not inspected.get("close_stdin"),
                            "A live native background process was mistaken for completed work")
                    time.sleep(0.2)
                    require(process.poll() is None, "Native background work exited before host cancellation")
                    # Exercise a deliberately short host deadline after the
                    # native yield, while the actual 30-second job is alive.
                    persistent_stopping = True
                    process.stdin.close()
            exit_code = process.wait(timeout=5)
            if self.name == "persistent":
                require(persistent_stopping and len(self.model.requests) == 2,
                        "Persistent native job did not reach its pending-work boundary")
                result, timeout_request = self.inspect(state, final=True, exit_code=exit_code, timed_out=True)
                require(result.get("failure_reason") == "omp_timed_out", "Host deadline became successful completion")
                require(self.host("structured-session", 3, "inspect", timeout_request, "timeout") == result,
                        "Actual adapter lost the native pending-work timeout")
                require(exit_code == 0, "Native cancellation did not dispose the background process")
                return {"scenario": self.name, "exit_code": exit_code, "model_requests": 2,
                        "expected_failure": "omp_timed_out"}
            if self.name == "fallback-selected":
                require(selected_failure is not None, "Explicit task model selection silently accepted a fallback")
                require(self.model.requests and all(row["body"]["model"] == "model" for row in self.model.requests),
                        "Explicit task model selection allowed a request to the fallback model")
                require(exit_code == 0, f"Native shutdown after provider refusal exited with {exit_code}")
                require(self.host("structured-session", 3, "inspect", failure_request, "selected-refusal") == selected_failure,
                        "Actual adapter lost the selected model's provider refusal")
                return {"scenario": self.name, "exit_code": exit_code, "model_requests": len(self.model.requests),
                        "expected_failure": selected_failure["failure_reason"]}
            result, final_request = self.inspect(state, final=True, exit_code=exit_code)
            require(exit_code == 0, f"Native OMP exited with {exit_code}")
            require(result.get("complete") and not result.get("failure_reason"), f"Native result failed: {result}")
            require(unchunks(result["result_base64_chunks"]).decode() == ANSWER,
                    "Final SDK result differs from actual assistant output")
            if self.name == "structured":
                require(checked_serving_model, "OMP emitted no assistant message establishing the serving model")
            require(self.host("structured-session", 3, "inspect", final_request, "final") == result,
                    "Actual adapter inspect handler differs from its parser result")
            require(any(frame.get("type") == "prompt_result" and frame.get("id") == "lg-prompt" and
                        frame.get("status") == "completed" for frame in native_frames),
                    "Native prompt completion was not correlated")
            if self.name == "delegated":
                relay = [json.loads(line) for line in (self.root / "relay.jsonl").read_text().splitlines()]
                require(any(row.get("method") == "tools/call" for row in relay), "Native delegation never called the relay")
                require(2 <= len(self.model.requests) <= 3, "Delegation did not finish after the real tool result")
            elif self.name == "fallback-preserved":
                require(any(row["body"]["model"] == "fallback" for row in self.model.requests),
                        "Omitted task model selection replaced the user's native fallback policy")
                require(any(frame.get("type") == "retry_fallback_applied" for frame in native_frames),
                        "The native runtime did not report its fallback transition")
            elif self.name in OBSERVATION_SCENARIOS:
                require(HOST_CONTEXT in json.dumps(self.model.requests), "Host context never reached the actual native model request")
                observed = self.check_observations()
            elif self.name == "background":
                require(any(frame.get("type") == "prompt_result" and frame.get("sessionSettled") is False
                            for frame in native_frames), "Native background work did not keep the prompt unsettled")
                require(any(frame.get("type") == "session_settled" for frame in native_frames),
                        "Native background completion never settled the session")
                require(len(self.model.requests) >= 3, "Native background completion did not wake another agent turn")
            else:
                require(len(self.model.requests) == 1, "Unexpected model calls")
            return {"scenario": self.name, "exit_code": exit_code,
                    "model_requests": len(self.model.requests), "native_frames": len(native_frames),
                    **(observed if self.name in OBSERVATION_SCENARIOS else {})}
        finally:
            if process is not None and process.poll() is None:
                if os.name == "nt":
                    process.kill()
                else:
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            self.model.close()
            write_json(self.root / "model-requests.json", self.model.requests)
            write_json(self.root / "inspection-transcript.json", self.transcript)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    runtime = parser.add_mutually_exclusive_group(required=True)
    runtime.add_argument("--runtime", type=Path, help="Linux x86-64 npm prefix with official OMP and Bun")
    runtime.add_argument("--omp", type=Path, help="path to the standalone native omp executable")
    parser.add_argument("--driver", type=Path, required=True, help="built omp_rpc_tests executable")
    parser.add_argument("--package", type=Path, required=True, help="assembled logonomic.cli.omp package")
    parser.add_argument("--host-simulator", type=Path, required=True, help="SDK host simulator executable")
    parser.add_argument("--host-driver", type=Path, help="built omp_native_host_driver; required for private wake outcomes")
    parser.add_argument("--artifacts", type=Path, required=True, help="fresh native evidence directory")
    parser.add_argument("--scenario", choices=("structured", "profile", "delegated", "background", "persistent", "lifecycle",
        "child", "nested-child", "cwd", "initial-prompt", "wake", "idle-model", "compaction", "helper-custody",
        "fallback-selected", "fallback-preserved", "all"), default="all")
    arguments = parser.parse_args()
    arguments.platform_id = {("linux", "x86_64"): "linux-x86_64", ("win32", "amd64"): "windows-x86_64",
        ("darwin", "arm64"): "macos-arm64"}.get((sys.platform, platform.machine().lower()))
    require(arguments.platform_id, "Unsupported native add-on platform")
    if arguments.platform_id != "linux-x86_64":
        require(arguments.omp and arguments.scenario in ("structured", "profile"),
                "Select the structured or profile pipe scenario and supply the native executable")
    if arguments.omp is not None:
        arguments.omp = arguments.omp.resolve(strict=True)
        require(arguments.omp.name.lower() == ("omp.exe" if os.name == "nt" else "omp"),
                "The installed executable must use its discoverable omp launcher name")
    else:
        arguments.runtime = arguments.runtime.resolve(strict=True)
    for name in ("driver", "package", "host_simulator", "artifacts"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    if arguments.host_driver is not None:
        arguments.host_driver = arguments.host_driver.resolve()
    arguments.artifacts.mkdir(parents=True, exist_ok=False)
    record_metadata(arguments)
    results = []
    for scenario in ("structured", "profile", "delegated", "background", "persistent", "lifecycle", "cwd",
                     "initial-prompt", "wake", "idle-model", "compaction", "child", "nested-child", "helper-custody",
                     "fallback-selected", "fallback-preserved"):
        if arguments.scenario in (scenario, "all"):
            case = Native_case(arguments, scenario)
            if scenario == "helper-custody":
                try:
                    case.check_helper_custody()
                    result = {"scenario": scenario, "eof_cancellation": "passed", "deadline_cancellation": "passed"}
                finally:
                    case.model.close()
            elif scenario in ("initial-prompt", "wake"):
                result = case.run_initial_prompt()
            elif scenario in ("idle-model", "compaction", "cwd"):
                result = case.run_native_control()
            else:
                result = case.run(scenario == "delegated" or scenario.startswith("fallback-"))
            results.append(result)
            write_json(arguments.artifacts / "summary.json", results)
            print(json.dumps(result), flush=True)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--relay-fixture":
        relay_fixture()
    elif len(sys.argv) > 1 and sys.argv[1] == "--wrapper-fixture":
        wrapper_fixture()
    else:
        main()
