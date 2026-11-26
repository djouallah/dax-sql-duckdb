#!/usr/bin/env python3
"""Puts a deployed semantic model's model.bim in OneLake, for the Direct Lake test: its TMSL
definition from the Fabric API (getDefinition), written to a lakehouse's Files.

    onelake_bim.py <workspace> <semantic model> <lakehouse> <path under Files> [<local copy>]

The tokens are in FABRIC_TOKEN (https://api.fabric.microsoft.com) and STORAGE_TOKEN
(https://storage.azure.com).
"""
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request


def request(method, url, token, body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req) as res:
            return res.status, dict(res.headers), res.read()
    except urllib.error.HTTPError as e:
        raise SystemExit(f"{method} {url.split('?')[0]}: HTTP {e.code} {e.read()[:500]!r}")


def definition(workspace, model, token):
    url = f"https://api.fabric.microsoft.com/v1/workspaces/{workspace}/semanticModels/{model}/getDefinition?format=TMSL"
    status, headers, body = request("POST", url, token, b"", {"Content-Length": "0"})
    if status == 202:
        # A long-running operation: Fabric sometimes takes minutes; /result only exists once it succeeded.
        location = headers["Location"]
        deadline = time.time() + 900
        while True:
            time.sleep(int(headers.get("Retry-After", "5")))
            _, headers, state = request("GET", location, token)
            state = json.loads(state)
            if state["status"] == "Succeeded":
                break
            if state["status"] == "Failed" or time.time() > deadline:
                raise SystemExit(f"getDefinition {state['status']}: {state.get('error')}")
        _, _, body = request("GET", f"{location}/result", token)
    parts = json.loads(body)["definition"]["parts"]
    bim = next(p for p in parts if p["path"] == "model.bim")
    return base64.b64decode(bim["payload"])


def upload(workspace, lakehouse, path, data, token):
    url = f"https://onelake.dfs.fabric.microsoft.com/{workspace}/{lakehouse}/Files/{path}"
    version = {"x-ms-version": "2023-11-03"}
    request("PUT", f"{url}?resource=file", token, b"", {**version, "Content-Length": "0"})
    request("PATCH", f"{url}?action=append&position=0", token, data,
            {**version, "Content-Type": "application/octet-stream"})
    request("PATCH", f"{url}?action=flush&position={len(data)}", token, b"", {**version, "Content-Length": "0"})


def main():
    workspace, model, lakehouse, path = sys.argv[1:5]
    data = definition(workspace, model, os.environ["FABRIC_TOKEN"])
    if len(sys.argv) > 5:
        with open(sys.argv[5], "wb") as f:
            f.write(data)
    upload(workspace, lakehouse, path, data, os.environ["STORAGE_TOKEN"])
    tables = len(json.loads(data.decode("utf-8-sig"))["model"]["tables"])
    print(f"model.bim ({len(data)} bytes, {tables} tables) -> {lakehouse}/Files/{path}")


if __name__ == "__main__":
    main()
