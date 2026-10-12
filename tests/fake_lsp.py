"""fake_lsp.py -- a language server that knows nothing, predictably.

Molly's language-server client is tested against this rather than against a
real server, for three reasons: CI has no clangd; a real server's answers
change with its version and with what it has indexed; and the interesting
cases -- a server that crashes, writes garbage, insists on UTF-16 or asks the
client questions of its own -- are ones no real server does on request.

What it "knows", all of it computed from the text it was sent:

  - **Diagnostics.** Every `BAD` is an error, every `WARN` a warning, every
    `NOTE` an information, each spanning the word. Sent after every open and
    every change, so the client's idea of the text is what is checked.
  - **Definitions.** A word is defined on the first line, in any open document
    or in any file in the working directory, that contains `define <word>`.
  - **Hover.** `word` hovers as a Markdown paragraph naming it.
  - **Completion.** Every word in the document that starts with what is
    before the cursor, plus `fake_alpha` and `fake_beta`, after `.` too.

Behaviour is chosen by environment variables, so a test asks for a broken
server the same way it asks for a working one:

  FAKE_LSP_LOG=path       append one JSON line per message received
  FAKE_LSP_ENCODING=x     answer with this position encoding ("utf-16" makes
                          the client convert); default: the first one offered
  FAKE_LSP_CRASH=1        print a line to stderr and exit after initializing
  FAKE_LSP_GARBAGE=1      answer `initialize` with something that is not LSP
  FAKE_LSP_ASK=1          ask the client questions of its own once ready
  FAKE_LSP_LINKS=1        answer definitions with LocationLinks
  FAKE_LSP_SLOW=seconds   wait this long before answering a request
  FAKE_LSP_DOCCHANGES=1   answer renames with documentChanges, not changes
  FAKE_LSP_MOVE=1         make a rename also move a file, which molly refuses
  FAKE_LSP_NORENAME=1     answer renames with an error
  FAKE_LSP_SIGOFFSETS=1   give signature parameters as offsets, not strings
  FAKE_LSP_NORANGE=1      format whole documents only

Formatting: every line in scope loses its trailing blanks, and with
insertSpaces its leading tabs become tabSize spaces each -- a line per edit.

Signature help: inside an unclosed `name(`, the signature `name(int a, int b,
int c)` with the parameter the commas say is active.

References are every whole-word occurrence, as renames change them -- in the open documents and in the
files in the working directory that are not open.
"""

import json
import os
import re
import sys
import time

LOG = os.environ.get("FAKE_LSP_LOG")
docs = {}      # uri -> text
encoding = "utf-16"
next_id = 1000


def log(entry):
    if LOG:
        with open(LOG, "a") as f:
            f.write(json.dumps(entry) + "\n")


def send(msg):
    body = json.dumps(msg).encode("utf-8")
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()


def read():
    length = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        k, _, v = line.partition(b":")
        if k.lower() == b"content-length":
            length = int(v)
    return json.loads(sys.stdin.buffer.read(length))


# Positions: a column is in code units of the negotiated encoding.

def units(s):
    if encoding == "utf-8":
        return len(s.encode("utf-8"))
    if encoding == "utf-32":
        return len(s)
    return len(s.encode("utf-16-le")) // 2


def col_of(line, n):
    """The str index that `n` units into `line` reaches."""
    for i in range(len(line) + 1):
        if units(line[:i]) >= n:
            return i
    return len(line)


def word_at(text, line, ch):
    lines = text.split("\n")
    if line >= len(lines):
        return "", 0, ""
    s = lines[line]
    i = col_of(s, ch)
    a = i
    while a > 0 and (s[a - 1].isalnum() or s[a - 1] == "_"):
        a -= 1
    b = i
    while b < len(s) and (s[b].isalnum() or s[b] == "_"):
        b += 1
    return s[a:b], a, s[a:i]


def diagnostics(uri):
    out = []
    for n, s in enumerate(docs[uri].split("\n")):
        for word, sev in (("BAD", 1), ("WARN", 2), ("NOTE", 3)):
            for m in re.finditer(r"\b%s\b" % word, s):
                out.append({
                    "range": {"start": {"line": n, "character": units(s[:m.start()])},
                              "end": {"line": n, "character": units(s[:m.end()])}},
                    "severity": sev,
                    "source": "fake",
                    "message": "%s is not allowed here" % word,
                })
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
          "params": {"uri": uri, "diagnostics": out}})


def find_definition(word):
    def search(uri, text):
        for n, s in enumerate(text.split("\n")):
            m = re.search(r"define %s\b" % re.escape(word), s)
            if m:
                start = m.start() + len("define ")
                return {"uri": uri,
                        "range": {"start": {"line": n, "character": units(s[:start])},
                                  "end": {"line": n, "character": units(s[:start + len(word)])}}}
        return None
    for uri, text in docs.items():
        hit = search(uri, text)
        if hit:
            return hit
    for name in sorted(os.listdir(".")):
        if not os.path.isfile(name):
            continue
        try:
            with open(name, encoding="utf-8") as f:
                text = f.read()
        except (OSError, UnicodeDecodeError):
            continue
        hit = search("file://" + os.path.abspath(name), text)
        if hit:
            return hit
    return None


def word_edits(text, word, new):
    out = []
    for n, s in enumerate(text.split("\n")):
        for m in re.finditer(r"\b%s\b" % re.escape(word), s):
            out.append({"range": {"start": {"line": n, "character": units(s[:m.start()])},
                                  "end": {"line": n, "character": units(s[:m.end()])}},
                        "newText": new})
    return out


def rename(word, new):
    if os.environ.get("FAKE_LSP_NORENAME"):
        raise ValueError("cannot rename that")
    by_uri = {}
    for uri, text in docs.items():
        e = word_edits(text, word, new)
        if e:
            by_uri[uri] = e
    for name in sorted(os.listdir(".")):
        uri = "file://" + os.path.abspath(name)
        if uri in docs or not os.path.isfile(name) or not name.endswith(".c"):
            continue
        with open(name, encoding="utf-8") as f:
            e = word_edits(f.read(), word, new)
        if e:
            by_uri[uri] = e
    if os.environ.get("FAKE_LSP_DOCCHANGES") or os.environ.get("FAKE_LSP_MOVE"):
        dc = [{"textDocument": {"uri": u, "version": None}, "edits": e}
              for u, e in by_uri.items()]
        if os.environ.get("FAKE_LSP_MOVE"):
            dc.append({"kind": "rename", "oldUri": "file:///a", "newUri": "file:///b"})
        return {"documentChanges": dc}
    return {"changes": by_uri}


def signature(text, line, ch):
    lines = text.split("\n")
    if line >= len(lines):
        return None
    before = lines[line][:col_of(lines[line], ch)]
    depth, i = 0, len(before) - 1
    while i >= 0:
        if before[i] == ")":
            depth += 1
        elif before[i] == "(":
            if depth == 0:
                break
            depth -= 1
        i -= 1
    if i < 0:
        return None
    m = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*$", before[:i])
    if not m:
        return None
    name = m.group(1)
    active = before[i + 1:].count(",")
    label = "%s(int a, int b, int c)" % name
    params = []
    for p in ("int a", "int b", "int c"):
        if os.environ.get("FAKE_LSP_SIGOFFSETS"):
            start = label.index(p)
            params.append({"label": [start, start + len(p)], "documentation": "the %s" % p[-1]})
        else:
            params.append({"label": p, "documentation": "the %s" % p[-1]})
    return {"signatures": [{"label": label, "parameters": params,
                            "documentation": "Does something with three."}],
            "activeSignature": 0, "activeParameter": min(active, 2)}


def formatting(text, opts, first=0, last=None):
    out = []
    lines = text.split("\n")
    if last is None:
        last = len(lines) - 1
    for n in range(first, min(last, len(lines) - 1) + 1):
        s = lines[n]
        new = s.rstrip(" \t")
        if opts.get("insertSpaces"):
            lead = len(new) - len(new.lstrip("\t"))
            new = " " * (lead * opts.get("tabSize", 8)) + new[lead:]
        if new != s:
            out.append({"range": {"start": {"line": n, "character": 0},
                                  "end": {"line": n, "character": units(s)}},
                        "newText": new})
    return out


def answer(msg):
    method = msg["method"]
    p = msg.get("params", {})
    if os.environ.get("FAKE_LSP_SLOW"):
        time.sleep(float(os.environ["FAKE_LSP_SLOW"]))
    if method == "shutdown":
        return None
    uri = p.get("textDocument", {}).get("uri")
    pos = p.get("position", {})
    if uri not in docs:
        return None
    word, start, before = word_at(docs[uri], pos.get("line", 0), pos.get("character", 0))
    if method == "textDocument/definition":
        loc = find_definition(word) if word else None
        if loc and os.environ.get("FAKE_LSP_LINKS"):
            return [{"targetUri": loc["uri"], "targetRange": loc["range"],
                     "targetSelectionRange": loc["range"]}]
        return loc
    if method == "textDocument/formatting":
        return formatting(docs[uri], p.get("options", {}))
    if method == "textDocument/rangeFormatting":
        r = p["range"]
        last = r["end"]["line"]
        # A range that ends at the start of a line does not take that line in.
        if r["end"]["character"] == 0 and last > r["start"]["line"]:
            last -= 1
        return formatting(docs[uri], p.get("options", {}), r["start"]["line"], last)
    if method == "textDocument/signatureHelp":
        return signature(docs[uri], pos.get("line", 0), pos.get("character", 0))
    if method == "textDocument/references":
        if not word:
            return []
        out = []
        for u, e in rename(word, word).get("changes", {}).items():
            for ed in e:
                out.append({"uri": u, "range": ed["range"]})
        return out
    if method == "textDocument/rename":
        return rename(word, p["newName"]) if word else None
    if method == "textDocument/hover":
        if not word:
            return None
        return {"contents": {"kind": "markdown",
                             "value": "```c\nint %s\n```\n\nThe thing called **%s**." % (word, word)}}
    if method == "textDocument/completion":
        words = sorted(set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", docs[uri])))
        words += ["fake_alpha", "fake_beta"]
        items = [{"label": w, "kind": 6, "detail": "fake"}
                 for w in words if w.startswith(before) and w != before]
        return {"isIncomplete": False, "items": items}
    return None


def main():
    global encoding, next_id
    while True:
        msg = read()
        if msg is None:
            return
        method = msg.get("method")
        entry = {"method": method} if method else {"response": msg.get("id")}
        if method in ("textDocument/didOpen", "textDocument/didChange"):
            td = msg["params"]["textDocument"]
            text = (td.get("text") if method.endswith("didOpen")
                    else msg["params"]["contentChanges"][-1]["text"])
            entry.update(uri=td["uri"], version=td.get("version"), text=text,
                         languageId=td.get("languageId"))
        elif method in ("textDocument/didSave", "textDocument/didClose"):
            entry.update(uri=msg["params"]["textDocument"]["uri"])
        elif method:
            entry.update(params=msg.get("params"))
        else:
            entry.update(result=msg.get("result"))
        log(entry)

        if method == "initialize":
            if os.environ.get("FAKE_LSP_GARBAGE"):
                sys.stdout.buffer.write(b"this is not a header\r\n" * 1000)
                sys.stdout.buffer.flush()
                time.sleep(5)
                return
            caps = msg["params"]["capabilities"]
            offered = caps.get("general", {}).get("positionEncodings", ["utf-16"])
            encoding = os.environ.get("FAKE_LSP_ENCODING", offered[0])
            send({"jsonrpc": "2.0", "id": msg["id"], "result": {
                "capabilities": {
                    "positionEncoding": encoding,
                    "textDocumentSync": {"openClose": True, "change": 1, "save": True},
                    "hoverProvider": True,
                    "definitionProvider": True,
                    "completionProvider": {"triggerCharacters": [".", "->"]},
                    "renameProvider": True,
                    "referencesProvider": True,
                    "signatureHelpProvider": {"triggerCharacters": ["(", ","]},
                    "documentFormattingProvider": True,
                    "documentRangeFormattingProvider": not os.environ.get("FAKE_LSP_NORANGE"),
                },
                "serverInfo": {"name": "fake"}}})
        elif method == "initialized":
            if os.environ.get("FAKE_LSP_CRASH"):
                sys.stderr.write("fake_lsp: something went\nfake_lsp: badly wrong\n")
                sys.stderr.flush()
                return
            if os.environ.get("FAKE_LSP_ASK"):
                send({"jsonrpc": "2.0", "id": next_id, "method": "workspace/configuration",
                      "params": {"items": [{"section": "a"}, {"section": "b"}]}})
                next_id += 1
                send({"jsonrpc": "2.0", "id": "tok", "method": "window/workDoneProgress/create",
                      "params": {"token": "x"}})
                send({"jsonrpc": "2.0", "method": "window/logMessage",
                      "params": {"type": 3, "message": "hello"}})
        elif method == "exit":
            return
        elif method == "textDocument/didOpen":
            docs[msg["params"]["textDocument"]["uri"]] = msg["params"]["textDocument"]["text"]
            diagnostics(msg["params"]["textDocument"]["uri"])
        elif method == "textDocument/didChange":
            uri = msg["params"]["textDocument"]["uri"]
            docs[uri] = msg["params"]["contentChanges"][-1]["text"]
            diagnostics(uri)
        elif method == "textDocument/didClose":
            docs.pop(msg["params"]["textDocument"]["uri"], None)
        elif "id" in msg and method:
            try:
                send({"jsonrpc": "2.0", "id": msg["id"], "result": answer(msg)})
            except ValueError as e:
                send({"jsonrpc": "2.0", "id": msg["id"],
                      "error": {"code": -32803, "message": str(e)}})


main()
