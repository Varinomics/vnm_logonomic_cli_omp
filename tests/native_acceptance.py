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
import subprocess
import sys
import threading
import time


ANSWER = "Native OMP completed the isolated fixture."
PROMPT = "Complete the isolated local fixture and report its result."
HOST_CONTEXT = "Include the isolated Logonomic host context in this native turn."
MODEL = "logonomic-fixture/model"
BASE_ENVIRONMENT = {"PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
                    "XDG_CONFIG_HOME", "XDG_DATA_HOME"}


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
    cli = arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent"
    bun = arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"
    files = {"harness": Path(__file__), "driver": arguments.driver,
        "host_simulator": arguments.host_simulator,
        "native_cli": cli / "dist/cli.js", "bun": bun}
    files.update({"package/" + str(path.relative_to(arguments.package)): path
        for path in arguments.package.rglob("*") if path.is_file()})
    write_json(arguments.artifacts / "metadata.json", {
        "platform": platform.platform(), "python": platform.python_version(),
        "native_package": json.loads((cli / "package.json").read_text())["version"],
        "bun": subprocess.check_output([str(bun), "--version"], text=True, timeout=5).strip(),
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
                if scenario == "delegated" and "Host accepted the fixture result." not in json.dumps(tool_results):
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
                    content = "Native work is still pending." if scenario in ("background", "persistent") and len(fixture.requests) == 2 else ANSWER
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
        bun = arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"
        cli = arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent/dist/cli.js"
        require(bun.is_file() and cli.is_file(), "Install the official OMP and Bun packages before this gate")
        launcher = self.root / "bin/omp"
        launcher.write_text(f"#!/bin/sh\nexec {shlex.quote(str(bun))} {shlex.quote(str(cli))} \"$@\"\n")
        launcher.chmod(0o755)
        self.environment = {"PATH": str(self.root / "bin") + ":/usr/bin:/bin", "LANG": "C.UTF-8",
            "HOME": str(self.root / "home"), "PI_CODING_AGENT_DIR": str(self.root / "agent"),
            "PI_NATIVES_DIR": str(self.root / "natives"), "PI_NO_TITLE": "1", "PI_NO_PTY": "1"}
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
                for model in (("model", "fallback") if name.startswith("fallback-") else ("model",))]}}})
        settings = {"enabledProviders": ["logonomic-fixture"],
            "telemetry": {"otlpExportEnabled": False}, "marketplace": {"autoUpdate": "off"},
            "power": {"sleepPrevention": "off"}}
        if name in ("background", "persistent"):
            settings["async"] = {"enabled": True}
        if name.startswith("fallback-"):
            settings["retry"] = {"enabled": True, "maxRetries": 0, "baseDelayMs": 1, "maxDelayMs": 50,
                "modelFallback": True, "rateLimitWalksChain": True,
                "fallbackChains": {MODEL: ["logonomic-fixture/fallback"]}}
        write_json(agent_directory / "config.yml", settings)
        self.argv = ["omp", "--no-session", "--model", MODEL]
        self.transcript = []

    def helper_environment(self, name="wrapper", stall=False):
        descriptor = self.root / f"{name}-descriptor.json"
        write_json(descriptor, {"log": str(self.root / f"{name}.jsonl"), "stall": stall})
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
            "launch_environment": {"platform": "linux-x86_64", "effective_environment":
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
        script = {"script_version": 1, "steps": [{"op": "invoke", "id": "request",
            "capability_id": capability, "version": version, "operation": operation, "payload": payload},
            {"op": "wait_result", "id": "request", "timeout_ms": 8000}, {"op": "shutdown"}]}
        path = self.root / f"host-{name}.json"
        write_json(path, script)
        result = subprocess.run([str(self.arguments.host_simulator), str(self.arguments.package),
            "--script", str(path), "--platform", "linux-x86_64"], capture_output=True, text=True, timeout=15)
        (self.root / f"host-{name}.txt").write_text(result.stdout + result.stderr)
        require(result.returncode == 0, f"SDK host simulator failed: {result.stderr}")
        frames = [json.loads(line[2:]) for line in result.stdout.splitlines() if line.startswith("< ")]
        terminal = next((frame for frame in frames if frame.get("type") == "result"), None)
        require(terminal is not None and terminal.get("status") == "success", f"Adapter rejected {name}: {terminal}")
        return terminal["payload"]

    def inspect(self, state, output=b"", error=b"", final=False, exit_code=0, timed_out=False):
        request = {"purpose": "implementation", "failure_diagnostics": {"version": 1},
            "inspection": {"protocol_state_base64_chunks": state,
            "stdout_base64_chunks": chunks(output), "stderr_base64_chunks": chunks(error),
            "final": final, "timed_out": timed_out, "exit_code": exit_code}}
        result = subprocess.run([str(self.arguments.driver), "--inspect"], input=json.dumps(request),
            text=True, capture_output=True, timeout=5)
        require(result.returncode == 0, f"Candidate inspection driver failed: {result.stderr}")
        response = json.loads(result.stdout)
        self.transcript.append({"inspection": request["inspection"], "result": response})
        return response, request

    def run(self, delegated):
        process = None
        try:
            discovered = self.host("launcher-discovery", 1, "discover", {"platform": "linux-x86_64",
                "ambient_environment": self.frozen_environment("launcher-discovery", 1),
                "default_home_fallback": str(self.root / "home")}, "discover")
            require(any(row.get("script_body") == "omp" for row in discovered["candidates"]),
                    "Actual adapter did not discover the installed native launcher")
            capability = "delegated-session" if delegated else "structured-session"
            request = self.prepare_request(delegated)
            if self.name == "lifecycle":
                interactive = {key: value for key, value in request.items() if key not in ("purpose", "prompt_base64")}
                interactive["declared_assets"] = ["assets/logonomic.ts"]
                interactive["title_instruction"] = HOST_CONTEXT
                interactive["launch_environment"]["effective_environment"] = self.frozen_environment("launch-preparation", 3)
                patch = self.host("launch-preparation", 3, "prepare", interactive, "interactive")
                require(patch.get("disposition", {}).get("lifecycle_events") is True,
                        "Native interactive launcher did not adopt packaged lifecycle hooks")
                injected = [argument.replace("{{asset:assets/logonomic.ts}}",
                    str(self.arguments.package / "assets/logonomic.ts")) for argument in patch["argv"]]
                self.argv = injected
                request = self.prepare_request(False)
            prepared = self.host(capability, 3, "prepare", request, "prepare")
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
            if self.name == "lifecycle":
                environment.update(self.helper_environment())
            command = [str(self.arguments.package / proposal["executable"]), *proposal["argv"]]
            process = subprocess.Popen(command, cwd=self.root / "workspace", env=environment,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
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
            elif self.name == "lifecycle":
                require(HOST_CONTEXT in json.dumps(self.model.requests), "Host context never reached the actual native model request")
                journal = [json.loads(line) for line in (self.root / "wrapper.jsonl").read_text().splitlines()]
                events = [row["body"]["event"] for row in journal if row["argv"][:2] == ["provider-event", "deliver"]]
                require([event["name"] for event in events] == ["session_start", "agent_start", "agent_end", "session_shutdown"],
                        f"Native extension lifecycle differs from its supported root contract: {events}")
                start = events[0]["body"]
                ledger = {"current_epoch": 0, "current_transition": None}
                classifications = []
                for index, event in enumerate(events):
                    classification = self.host("helper-ingress", 2, "classify_event", {
                        "event": event, "ledger": ledger, "transition_nonce": event["body"]["activation_nonce"]},
                        f"native-event-{index}")
                    classifications.append(classification)
                    ledger = {"current_epoch": 1, "current_transition": {"kind": "start",
                        "provider_session_id": start["session_id"], "nonce": start["activation_nonce"]}}
                write_json(self.root / "native-classifications.json", classifications)
                require(any(row.get("outcome_transition") == "succeeded"
                            for result in classifications for row in result.get("observations", [])),
                        "Actual native completion never became a host lifecycle success")
            elif self.name == "background":
                require(any(frame.get("type") == "prompt_result" and frame.get("sessionSettled") is False
                            for frame in native_frames), "Native background work did not keep the prompt unsettled")
                require(any(frame.get("type") == "session_settled" for frame in native_frames),
                        "Native background completion never settled the session")
                require(len(self.model.requests) >= 3, "Native background completion did not wake another agent turn")
            else:
                require(len(self.model.requests) == 1, "Unexpected model calls")
            return {"scenario": self.name, "exit_code": exit_code,
                    "model_requests": len(self.model.requests), "native_frames": len(native_frames)}
        finally:
            if process is not None and process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            self.model.close()
            write_json(self.root / "model-requests.json", self.model.requests)
            write_json(self.root / "inspection-transcript.json", self.transcript)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True, help="npm prefix with official OMP and Bun")
    parser.add_argument("--driver", type=Path, required=True, help="built omp_rpc_tests executable")
    parser.add_argument("--package", type=Path, required=True, help="assembled logonomic.cli.omp package")
    parser.add_argument("--host-simulator", type=Path, required=True, help="SDK host simulator executable")
    parser.add_argument("--artifacts", type=Path, required=True, help="fresh native evidence directory")
    parser.add_argument("--scenario", choices=("structured", "profile", "delegated", "background", "persistent", "lifecycle",
        "helper-custody", "fallback-selected", "fallback-preserved", "all"), default="all")
    arguments = parser.parse_args()
    require(sys.platform == "linux" and platform.machine() == "x86_64",
            "This native acceptance harness requires Linux x86-64")
    for name in ("runtime", "driver", "package", "host_simulator", "artifacts"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    arguments.artifacts.mkdir(parents=True, exist_ok=False)
    record_metadata(arguments)
    results = []
    for scenario in ("structured", "profile", "delegated", "background", "persistent", "lifecycle", "helper-custody",
                     "fallback-selected", "fallback-preserved"):
        if arguments.scenario in (scenario, "all"):
            case = Native_case(arguments, scenario)
            if scenario == "helper-custody":
                try:
                    case.check_helper_custody()
                    result = {"scenario": scenario, "eof_cancellation": "passed", "deadline_cancellation": "passed"}
                finally:
                    case.model.close()
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
