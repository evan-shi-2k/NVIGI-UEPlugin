#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
nim_structured.py
- Single-shot or persistent stdin-serving wrapper around OpenAI Python SDK (xgrammar-capable).
- Designed to talk to a locally hosted NIM (OpenAI-compatible) endpoint or OpenAI directly.
- Keeps grammar/schema and client warm when run with --serve-stdin to minimize latency.

Protocol for --serve-stdin:
- Read one line per request from STDIN (newline-delimited JSON).
- Expected minimal payload: {"user": "<prompt text>"}
- Optional overrides: "system", "assistant"
- Control: {"__cmd":"ping"} -> {"ok":true,"pong":true}
- Quit: {"__cmd":"quit"} or EOF
- Respond with a single-line JSON string (no trailing logs), newline-delimited, then flush.
"""

import sys, os, json, argparse, traceback
from typing import Optional, Dict, Any, Tuple

# NOTE: We rely on the OpenAI Python SDK that supports xgrammar via extra_body.
try:
    from openai import OpenAI
except Exception:
    sys.stderr.write("FATAL: openai package not found. Install with `pip install openai`.\n")
    raise

DEFAULT_BASE_URL = os.environ.get("NIM_BASE_URL", "http://127.0.0.1:8000/v1")
DEFAULT_MODEL    = os.environ.get("NIM_MODEL_NAME", "meta/llama-3.2-3b-instruct")
DEFAULT_API_KEY  = os.environ.get("NIM_API_KEY", "not-used")

def configure_stdio():
    """
    Make stdout newline-framed + reliably flushed.
    - Python is launched with -u on your side already, but we also:
      * set UTF-8 where available
      * enable line buffering where available
    """
    try:
        sys.stdin.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    except Exception:
        pass
    try:
        sys.stderr.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    except Exception:
        pass

def send_json(obj: Any):
    """
    The ONLY function that writes to stdout.
    Always emits exactly one JSON line terminated by \\n, then flushes.
    """
    try:
        line = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
    except Exception as e:
        line = json.dumps({"error": "json_dumps_failed", "detail": str(e)}, separators=(",", ":"))
    sys.stdout.write(line + "\n")
    sys.stdout.flush()

def read_text(path: Optional[str]) -> Optional[str]:
    if not path:
        return None
    with open(path, "r", encoding="utf-8") as f:
        return f.read()

def read_json(path: Optional[str]) -> Optional[Dict[str, Any]]:
    if not path:
        return None
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

class StructuredClient:
    def __init__(
        self,
        base_url: str,
        api_key: str,
        model: str,
        mode: str,
        system_path: Optional[str],
        assistant_path: Optional[str],
        grammar_path: Optional[str],
        json_schema_path: Optional[str],
        temperature: float = 0.0,
        max_tokens: Optional[int] = None
    ):
        self.client = OpenAI(base_url=base_url, api_key=api_key)
        self.model = model
        self.mode = mode
        self.temperature = temperature
        self.max_tokens = max_tokens

        # Load once (warm)
        self.system_text = read_text(system_path)
        self.assistant_text = read_text(assistant_path)

        self._grammar_cache: Dict[str, Tuple[float, str]] = {}
        self._schema_cache: Dict[str, Tuple[float, Dict[str, Any]]] = {}

        if mode == "grammar":
            self.grammar = read_text(grammar_path)
            if not self.grammar:
                raise ValueError("--grammar is required in grammar mode")
            self.schema = None
        elif mode == "json":
            self.schema = read_json(json_schema_path)
            if not self.schema:
                raise ValueError("--json-schema is required in json mode")
            self.grammar = None
        else:
            raise ValueError("mode must be either 'grammar' or 'json'")

        sys.stderr.write(f"[nim_structured] Initialized (model={self.model}, mode={self.mode})\n")

    def build_messages(self, user: str, system_override: Optional[str], assistant_override: Optional[str]):
        msgs = []
        sys_prompt = system_override if system_override is not None else self.system_text
        asst_prompt = assistant_override if assistant_override is not None else self.assistant_text
        if sys_prompt:
            msgs.append({"role": "system", "content": sys_prompt})
        if asst_prompt:
            msgs.append({"role": "assistant", "content": asst_prompt})
        msgs.append({"role": "user", "content": user})
        return msgs

    def _get_grammar_from_path(self, path: str) -> str:
        """Load grammar from disk with a tiny mtime cache."""
        st = os.stat(path)
        mtime = float(st.st_mtime)
        cached = self._grammar_cache.get(path)
        if cached and cached[0] == mtime:
            return cached[1]
        text = read_text(path)
        if not text:
            raise ValueError(f"grammar file empty or unreadable: {path}")
        self._grammar_cache[path] = (mtime, text)
        return text

    def _get_schema_from_path(self, path: str) -> Dict[str, Any]:
        """Load JSON schema from disk with a tiny mtime cache."""
        st = os.stat(path)
        mtime = float(st.st_mtime)
        cached = self._schema_cache.get(path)
        if cached and cached[0] == mtime:
            return cached[1]
        data = read_json(path)
        if not data:
            raise ValueError(f"json schema file empty or unreadable: {path}")
        self._schema_cache[path] = (mtime, data)
        return data

    def infer(
        self,
        user: str,
        system_override: Optional[str] = None,
        assistant_override: Optional[str] = None,
        grammar_path_override: Optional[str] = None,
        grammar_text_override: Optional[str] = None,
        json_schema_path_override: Optional[str] = None,
        json_schema_override: Optional[Dict[str, Any]] = None,
    ) -> Any:
        if self.mode == "grammar":
            g = self.grammar
            if grammar_text_override is not None:
                g = grammar_text_override
            elif grammar_path_override:
                g = self._get_grammar_from_path(grammar_path_override)
            extra = {"guided_grammar": g}
        else:
            s = self.schema
            if json_schema_override is not None:
                s = json_schema_override
            elif json_schema_path_override:
                s = self._get_schema_from_path(json_schema_path_override)
            extra = {"guided_json": s}

        kwargs = dict(
            model=self.model,
            messages=self.build_messages(user, system_override, assistant_override),
            temperature=self.temperature,
            extra_body=extra
        )
        if self.max_tokens is not None:
            kwargs["max_tokens"] = self.max_tokens

        resp = self.client.chat.completions.create(**kwargs)
        content = (resp.choices[0].message.content or "").strip()
        try:
            return json.loads(content)
        except Exception:
            return {"error": "non_json_output", "detail": content}

def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Structured-output client with optional stdin server loop.")
    p.add_argument("--base-url", default=DEFAULT_BASE_URL, help="OpenAI-compatible base URL (NIM gateway)")
    p.add_argument("--api-key", default=DEFAULT_API_KEY, help="API key (may be unused for local NIM)")
    p.add_argument("--model", default=DEFAULT_MODEL, help="Model name")
    p.add_argument("--mode", choices=["grammar", "json"], default="grammar", help="Structured mode")
    p.add_argument("--system", dest="system_path", help="Path to system prompt file")
    p.add_argument("--assistant", dest="assistant_path", help="Path to assistant prompt file")
    p.add_argument("--grammar", dest="grammar_path", help="Path to grammar file (mode=grammar)")
    p.add_argument("--json-schema", dest="json_schema_path", help="Path to JSON schema file (mode=json)")
    p.add_argument("--temperature", type=float, default=0.0)
    p.add_argument("--max-tokens", type=int, default=None)
    p.add_argument("--serve-stdin", action="store_true", help="Run persistent stdin server")
    p.add_argument("--user", dest="user_prompt", help="Single-shot: user prompt text")
    return p.parse_args(argv)

def one_shot(sc: StructuredClient, user: str, system: Optional[str], assistant: Optional[str]) -> int:
    try:
        out = sc.infer(user, system_override=system, assistant_override=assistant)
        send_json(out)
        return 0
    except Exception as e:
        sys.stderr.write("ERROR(one_shot): " + repr(e) + "\n")
        traceback.print_exc(file=sys.stderr)
        send_json({"error": "exception", "detail": str(e)})
        return 2

def serve_stdin(sc: StructuredClient) -> int:
    if os.getenv("NIM_STRUCTURED_BANNER", "0") == "1":
        sys.stderr.write("[nim_structured] Entering --serve-stdin loop. Send {\"user\":\"...\"}\\n per request.\n")

    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line:
            continue

        try:
            req = json.loads(line)
        except Exception:
            send_json({"error": "bad_request", "detail": "invalid json line"})
            continue

        if isinstance(req, dict) and "__cmd" in req:
            cmd = req["__cmd"]
            if cmd == "ping":
                send_json({"ok": True, "pong": True})
                continue
            if cmd in ("quit", "exit", "stop"):
                send_json({"ok": True, "bye": True})
                break

        try:
            if not isinstance(req, dict) or "user" not in req or not isinstance(req["user"], str):
                send_json({"error": "bad_request", "detail": "missing 'user' string"})
                continue

            user = req["user"]
            sys_override = req.get("system")
            asst_override = req.get("assistant")

            grammar_path = req.get("grammar_path")
            grammar_text = req.get("grammar")
            schema_path = req.get("json_schema_path")
            schema_obj = req.get("json_schema")

            out = sc.infer(
                user,
                system_override=sys_override,
                assistant_override=asst_override,
                grammar_path_override=grammar_path,
                grammar_text_override=grammar_text,
                json_schema_path_override=schema_path,
                json_schema_override=schema_obj,
            )
            send_json(out)

        except Exception as e:
            sys.stderr.write("ERROR(serve): " + repr(e) + "\n")
            traceback.print_exc(file=sys.stderr)
            send_json({"error": "exception", "detail": str(e)})

    sys.stderr.write("[nim_structured] Exiting --serve-stdin.\n")
    return 0

def main(argv=None) -> int:
    configure_stdio()
    args = parse_args(argv)

    try:
        sc = StructuredClient(
            base_url=args.base_url,
            api_key=args.api_key,
            model=args.model,
            mode=args.mode,
            system_path=args.system_path,
            assistant_path=args.assistant_path,
            grammar_path=args.grammar_path,
            json_schema_path=args.json_schema_path,
            temperature=args.temperature,
            max_tokens=args.max_tokens,
        )
    except Exception as e:
        sys.stderr.write("FATAL(init): " + str(e) + "\n")
        return 2

    if args.serve_stdin:
        return serve_stdin(sc)

    if not args.user_prompt:
        sys.stderr.write("Single-shot mode requires --user '<prompt>'\n")
        return 2

    return one_shot(sc, args.user_prompt, None, None)

if __name__ == "__main__":
    sys.exit(main())
