"""Local host wrapper invoked once per helper request under its native deadline."""

import json
import os
import sys
import time


def main():
    with open(sys.argv[1], encoding="utf8") as file:
        descriptor = json.load(file)
    arguments = sys.argv[2:]
    body = json.load(sys.stdin) if "--args-stdin" in arguments else None
    with open(descriptor["log"], "a", encoding="utf8") as log:
        log.write(json.dumps({"argv": arguments, "body": body, "pid": os.getpid()}) + "\n")
    if descriptor["stall"]:
        time.sleep(60)
        return
    if arguments[0] == "request-title-instruction":
        result = {"accepted": True, "instruction_required": True, "instruction": descriptor["instruction"]}
    else:
        result = {"accepted": True, "epoch": 1}
        if descriptor["children"]:
            result["helper_response"] = {"child_state_version": 1}
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
