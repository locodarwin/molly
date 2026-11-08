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

Behavior is chosen by environment variables, so a test asks for a broken
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
  FAKE_LSP_SNIPPETS=1     also offer snippet completions, named snip_..., to a
                          client that says it takes snippets
  FAKE_LSP_SYNC=n         textDocumentSync change kind: 2, range edits, by
                          default; 1 for the whole document every time
  FAKE_LSP_MIRROR=path    write the document as it now stands to `path`, as
                          UTF-8, after every open and change

Range edits are applied strictly: a line past the end, a column past the end
of its line or inside a character is an error, logged and fatal, so a client
out of step shows up as a server that died rather than as quietly wrong text.
Lines end at "\r\n", "\r" or "\n", as the protocol says.

Formatting: every line in scope loses its trailing blanks, and with
insertSpaces its leading tabs become tabSize spaces each -- a line per edit.

Signature help: inside an unclosed `name(`, the signature `name(int a, int b,
int c)` with the parameter the commas say is active.

Code actions, at any place: "Upper-case this line", a command whose edit comes
back as the server's own workspace/applyEdit; "Add a comment", which has its
edit filled in by codeAction/resolve; and "Cannot do this", disabled. With a
BAD diagnostic in the context sent, "Replace BAD with GOOD" first, carrying
its edit.

References are every whole-word occurrence, as renames change them -- in the open documents and in the
files in the working directory that are not open.
"""

import json
import os
import re
import sys
import time

LOG = os.environ.get("FAKE_LSP_LOG")
MIRROR = os.environ.get("FAKE_LSP_MIRROR")
SYNC = int(os.environ.get("FAKE_LSP_SYNC", "2"))
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


LINE_BREAK = re.compile(r"\r\n|\r|\n")


def offset_of(text, pos):
    """The str index of a position in `text`, strictly."""
    line, ch = pos["line"], pos["character"]
    start = 0
    for _ in range(line):
        m = LINE_BREAK.search(text, start)
        if m is None:
            raise ValueError("line %d is past the end" % line)
        start = m.end()
    m = LINE_BREAK.search(text, start)
    end = m.start() if m else len(text)
    i, u = start, 0
    while u < ch:
        if i >= end:
            raise ValueError("column %d is past the end of line %d" % (ch, line))
        u += units(text[i])
        i += 1
    if u != ch:
        raise ValueError("column %d is inside a character on line %d" % (ch, line))
    return i


def apply_change(text, change):
    if "range" not in change:
        return change["text"]
    a = offset_of(text, change["range"]["start"])
    b = offset_of(text, change["range"]["end"])
    if b < a:
        raise ValueError("a range that ends before it starts")
    return text[:a] + change["text"] + text[b:]


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


def code_actions(uri, p):
    line = p["range"]["start"]["line"]
    out = []
    for d in p.get("context", {}).get("diagnostics", []):
        if d["message"].startswith("BAD"):
            out.append({"title": "Replace BAD with GOOD", "kind": "quickfix",
                        "diagnostics": [d],
                        "edit": {"changes": {uri: [{"range": d["range"], "newText": "GOOD"}]}}})
    out.append({"title": "Upper-case this line", "kind": "refactor.rewrite",
                "command": {"title": "Upper-case", "command": "fake.upper",
                            "arguments": [uri, line, {"nested": [1, True, None]}]}})
    out.append({"title": "Add a comment", "kind": "refactor", "data": {"line": line}})
    out.append({"title": "Cannot do this", "disabled": {"reason": "fake"}})
    return out


def answer(msg):
    global next_id
    method = msg["method"]
    p = msg.get("params", {})
    if os.environ.get("FAKE_LSP_SLOW"):
        time.sleep(float(os.environ["FAKE_LSP_SLOW"]))
    if method == "shutdown":
        return None
    if method == "codeAction/resolve":
        uri = list(docs)[0]
        line = p["data"]["line"]
        p["edit"] = {"changes": {uri: [{"range": {"start": {"line": line, "character": 0},
                                                  "end": {"line": line, "character": 0}},
                                        "newText": "// fake\n"}]}}
        return p
    if method == "workspace/executeCommand":
        uri, line, extra = p["arguments"]
        if extra != {"nested": [1, True, None]}:
            raise ValueError("the arguments came back changed")
        text = docs[uri].split("\n")[line]
        send({"jsonrpc": "2.0", "id": next_id, "method": "workspace/applyEdit",
              "params": {"edit": {"changes": {uri: [{
                  "range": {"start": {"line": line, "character": 0},
                            "end": {"line": line, "character": units(text)}},
                  "newText": text.upper()}]}}}})
        next_id += 1
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
    if method == "textDocument/codeAction":
        return code_actions(uri, p)
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
        if snippets and os.environ.get("FAKE_LSP_SNIPPETS"):
            items += [{"label": label, "kind": 15, "insertTextFormat": 2, "insertText": text}
                      for label, text in SNIPPETS if label.startswith(before)]
        return {"isIncomplete": False, "items": items}
    return None


# Snippet completions, by label: two fields and an end; one field twice; a
# placeholder inside another; several lines with a tab; and text that only
# looks like a field.
SNIPPETS = [
    ("snip_call", "snip_call(${1:int a}, ${2:int b})$0"),
    ("snip_twice", "${1:n} * $1;$0"),
    ("snip_nest", "f(${1:a, ${2:b}})$0"),
    ("snip_block", "if (${1:cond}) {\n\t$0\n}"),
    ("snip_money", "cost(\\$5, ${1|usd,eur|})"),
]
snippets = False


def main():
    global encoding, next_id, snippets
    while True:
        msg = read()
        if msg is None:
            return
        method = msg.get("method")
        entry = {"method": method} if method else {"response": msg.get("id")}
        if method in ("textDocument/didOpen", "textDocument/didChange"):
            td = msg["params"]["textDocument"]
            if method.endswith("didOpen"):
                text = td.get("text")
            else:
                text = docs.get(td["uri"], "")
                changes = msg["params"]["contentChanges"]
                entry.update(ranged=["range" in c for c in changes])
                try:
                    for change in changes:
                        text = apply_change(text, change)
                except (ValueError, KeyError, TypeError) as e:
                    entry.update(error=str(e))
                    log(entry)
                    raise
            docs[td["uri"]] = text
            # The document as it now stands, which for a range edit is not
            # what was sent: the checks are of what the server has.
            entry.update(uri=td["uri"], version=td.get("version"), text=text,
                         languageId=td.get("languageId"))
        elif method in ("textDocument/didSave", "textDocument/didClose"):
            entry.update(uri=msg["params"]["textDocument"]["uri"])
        elif method:
            entry.update(params=msg.get("params"))
        else:
            entry.update(result=msg.get("result"))
        log(entry)
        # After the log, so that a test which waits for the mirror finds the
        # log line already written.
        if MIRROR and method in ("textDocument/didOpen", "textDocument/didChange"):
            with open(MIRROR, "wb") as f:
                f.write(entry["text"].encode("utf-8"))

        if method == "initialize":
            if os.environ.get("FAKE_LSP_GARBAGE"):
                sys.stdout.buffer.write(b"this is not a header\r\n" * 1000)
                sys.stdout.buffer.flush()
                time.sleep(5)
                return
            caps = msg["params"]["capabilities"]
            snippets = (caps.get("textDocument", {}).get("completion", {})
                        .get("completionItem", {}).get("snippetSupport", False))
            offered = caps.get("general", {}).get("positionEncodings", ["utf-16"])
            encoding = os.environ.get("FAKE_LSP_ENCODING", offered[0])
            send({"jsonrpc": "2.0", "id": msg["id"], "result": {
                "capabilities": {
                    "positionEncoding": encoding,
                    "textDocumentSync": {"openClose": True, "change": SYNC, "save": True},
                    "hoverProvider": True,
                    "definitionProvider": True,
                    "completionProvider": {"triggerCharacters": [".", "->"]},
                    "renameProvider": True,
                    "referencesProvider": True,
                    "signatureHelpProvider": {"triggerCharacters": ["(", ","]},
                    "documentFormattingProvider": True,
                    "documentRangeFormattingProvider": not os.environ.get("FAKE_LSP_NORANGE"),
                    "codeActionProvider": {"resolveProvider": True},
                    "executeCommandProvider": {"commands": ["fake.upper"]},
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
        elif method in ("textDocument/didOpen", "textDocument/didChange"):
            diagnostics(msg["params"]["textDocument"]["uri"])
        elif method == "textDocument/didClose":
            docs.pop(msg["params"]["textDocument"]["uri"], None)
        elif "id" in msg and method:
            try:
                send({"jsonrpc": "2.0", "id": msg["id"], "result": answer(msg)})
            except ValueError as e:
                send({"jsonrpc": "2.0", "id": msg["id"],
                      "error": {"code": -32803, "message": str(e)}})


main()
