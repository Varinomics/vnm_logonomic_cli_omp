"""Opt-in native URI-routing regression for the read-only task boundary.

Use an isolated local model, an ambient MCP resource that would write a marker,
and a loopback HTTP target. The actual packaged guard must refuse every routed
read/search before either target is called, while local reads and the exact
delegated task action still execute. No real provider account is used.
"""

import argparse
import hashlib
import http.server
import json
from pathlib import Path
import re
import sys
import threading

import native_review_acceptance as review

native = review.native


def resource_server(directory):
    journal = directory / "resource-relay.jsonl"
    for line in sys.stdin:
        request = json.loads(line)
        with journal.open("a") as stream:
            stream.write(json.dumps(request) + "\n")
        if "id" not in request:
            continue
        method = request.get("method")
        if method == "initialize":
            result = {"protocolVersion": request["params"]["protocolVersion"],
                "capabilities": {"resources": {}, "tools": {}},
                "serverInfo": {"name": "isolated-uri-regression", "version": "1"}}
        elif method == "tools/list":
            result = {"tools": []}
        elif method == "resources/list":
            result = {"resources": [{"uri": uri, "name": "opaque resource", "mimeType": "text/plain"}
                for uri in ("reviewprobe:mutate", "reviewprobe:raw ", "reviewprobe:12 ")]}
        elif method == "resources/templates/list":
            result = {"resourceTemplates": []}
        elif method == "resources/read":
            (directory / "workspace/opaque-written.txt").write_text("Unexpected resource dispatch.\n")
            result = {"contents": [{"uri": request["params"]["uri"], "mimeType": "text/plain",
                "text": "OPAQUE_RESOURCE_EXECUTED"}]}
        elif method == "ping":
            result = {}
        else:
            print(json.dumps({"jsonrpc": "2.0", "id": request["id"],
                "error": {"code": -32601, "message": "Unknown fixture method"}}), flush=True)
            continue
        print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)


class RoutingModel:
    def __init__(self, delegated):
        native.require(delegated, "The routing gate includes an exact delegated action")
        self.requests, self.emitted, self.http_reads = [], [], []
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_arguments):
                pass

            def do_GET(self):
                fixture.http_reads.append({"method": self.command, "path": self.path})
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(b"UNEXPECTED_HTTP_READ\n")

            do_HEAD = do_GET

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                fixture.requests.append({"path": self.path, "body": body})
                native.require(self.path == "/v1/chat/completions", "Unexpected model route")
                index = len(fixture.requests) - 1
                target = f"http:/127.0.0.1:{fixture.server.server_port}/mutate"
                results = json.dumps([row for row in body["messages"] if row.get("role") == "tool"])
                names = re.findall(r"xd://([a-z0-9_]+_report_result)\b", results)
                action = names[0] if names else "missing"
                plan = [
                    ("read", {"path": "review.txt:1"}, True),
                    ("grep", {"pattern": "READ_ONLY_SOURCE_EVIDENCE", "path": "review.txt"}, True),
                    ("glob", {"path": "review.txt"}, True),
                    ("read", {"path": "reviewprobe:mutate"}, False),
                    ("read", {"path": "reviewprobe:raw "}, False),
                    ("read", {"path": "reviewprobe:12 "}, False),
                    ("read", {"path": target}, False),
                    ("read", {"path": target.replace("http:", "https:")}, False),
                    ("read", {"path": "local:/notes"}, False),
                    ("read", {"path": "/repo/local://notes"}, False),
                    ("read", {"path": "review.txt;reviewprobe:mutate"}, False),
                    ("read", {"path": "reviewprobe:mutate;review.txt"}, False),
                    ("read", {"path": 'review.txt;"reviewprobe:mutate"'}, False),
                    ("read", {"path": "review.txt;" + target}, False),
                    ("read", {"path": "Makefile:12 other"}, False),
                    ("grep", {"pattern": "x", "path": target}, False),
                    ("grep", {"pattern": "x", "path": "review.txt;local:/notes"}, False),
                    ("glob", {"path": "local:/notes"}, False),
                    ("glob", {"path": r"local:\notes"}, False),
                    ("glob", {"path": "/repo/local://notes"}, False),
                    ("read", {"path": "xd://"}, True),
                    ("read", {"path": "xd://" + action}, True),
                    ("write", {"path": "xd://" + action,
                        "content": json.dumps({"message": "native task result"})}, True),
                    ("write", {"path": "xd://" + action.removesuffix("report_result") + "send_message",
                        "content": json.dumps({"message": "forbidden action"})}, False),
                    ("read", {"path": "xd://" + action + ";reviewprobe:mutate"}, False),
                ]
                native.require(index <= len(plan), "The native routing review did not settle")
                if index < len(plan):
                    name, arguments, permitted = plan[index]
                    native.require("missing" not in arguments.get("path", ""), "Missing exact native xd inventory")
                    call_id = f"review-uri-{index}"
                    fixture.emitted.append({"id": call_id, "name": name, "input": arguments, "permitted": permitted})
                    delta = {"role": "assistant", "tool_calls": [{"index": 0, "id": call_id,
                        "type": "function", "function": {"name": name, "arguments": json.dumps(arguments)}}]}
                    finish = "tool_calls"
                else:
                    delta, finish = {"role": "assistant", "content": native.ANSWER}, "stop"
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                for value, reason in ((delta, None), ({}, finish)):
                    frame = {"id": "routing-completion", "object": "chat.completion.chunk", "created": 1,
                        "model": "model", "choices": [{"index": 0, "delta": value, "finish_reason": reason}]}
                    self.wfile.write(("data: " + json.dumps(frame) + "\n\n").encode())
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def main():
    parser = argparse.ArgumentParser()
    for name in ("runtime", "package", "driver", "host-driver", "artifacts"):
        parser.add_argument("--" + name, type=Path, required=True)
    arguments = parser.parse_args()
    for name in ("runtime", "package", "driver", "host_driver", "artifacts"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    arguments.artifacts.mkdir(exist_ok=False)
    files = {"harness": Path(__file__), "review_harness": Path(review.__file__),
        "fixture_library": Path(native.__file__), "driver": arguments.driver, "host_driver": arguments.host_driver,
        "native_cli": arguments.runtime / "node_modules/@oh-my-pi/pi-coding-agent/dist/cli.js",
        "bun": arguments.runtime / "node_modules/@oven/bun-linux-x64/bin/bun"}
    files.update({"package/" + str(path.relative_to(arguments.package)): path
        for path in arguments.package.rglob("*") if path.is_file()})
    native.write_json(arguments.artifacts / "metadata.json", {"file_sha256": {
        name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in sorted(files.items())},
        "model_transport": "isolated loopback fixture; no real account", "native_session_persistence": False})
    review.ReviewModel = RoutingModel
    case = review.ReviewCase(arguments, True)
    native.write_json(case.root / "agent/mcp.json", {"mcpServers": {"ambient-resource-probe": {
        "command": sys.executable, "args": [str(Path(__file__).resolve()), "--mcp", str(case.root)]}}})
    summary = case.run_review()
    frames = json.loads((case.root / "native-frames.json").read_text())
    tool_results = [row["message"] for row in frames if row.get("type") == "message_end" and
        row.get("message", {}).get("role") == "toolResult"]
    by_id = {row["toolCallId"]: row for row in tool_results}
    native.require(len(by_id) == len(tool_results) == len(case.model.emitted), "Missing or duplicate native tool results")
    for emitted in case.model.emitted:
        result = by_id[emitted["id"]]
        native.require(bool(result.get("isError")) is not emitted["permitted"], f"Wrong result for {emitted}: {result}")
        if not emitted["permitted"]:
            native.require("This Logonomic review permits native file reads" in json.dumps(result),
                f"Routed tool did not reach the guard refusal: {emitted}: {result}")
    journal = [json.loads(line) for line in (case.root / "resource-relay.jsonl").read_text().splitlines()]
    native.require(any(row.get("method") == "initialize" for row in journal), "Ambient MCP fixture was not initialized")
    resource_reads = [row for row in journal if row.get("method") == "resources/read"]
    native.require(not resource_reads, "Native tool reached ambient MCP resources/read")
    native.require(not (case.root / "workspace/opaque-written.txt").exists(), "Ambient MCP wrote a workspace marker")
    native.require(not case.model.http_reads, "Native tool reached the loopback HTTP resource")
    summary.update({"resource_reads": resource_reads, "http_reads": case.model.http_reads,
        "guard_refusals": sum(not row["permitted"] for row in case.model.emitted),
        "admitted_tool_results": sum(row["permitted"] for row in case.model.emitted)})
    native.write_json(arguments.artifacts / "calls.json", case.model.emitted)
    native.write_json(arguments.artifacts / "result.json", summary)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--mcp":
        resource_server(Path(sys.argv[2]))
    else:
        main()
