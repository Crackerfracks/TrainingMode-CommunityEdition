#!/usr/bin/env bash
# Install or refresh the kb wiring on this machine. Idempotent; backs up what it edits.
# Usage: tools/install.sh [--dry-run]
set -euo pipefail
KB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRY=${1:-}
say() { printf '%s\n' "$*"; }
run() { if [ "$DRY" = "--dry-run" ]; then say "would: $*"; else "$@"; fi; }
stamp=$(date +%Y%m%d-%H%M%S)

say "kb root: $KB_ROOT"

# 1. CLI on PATH
run mkdir -p "$HOME/.local/bin"
run ln -sfn "$KB_ROOT/tools/kb" "$HOME/.local/bin/kb"
case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) say "note: add ~/.local/bin to PATH (or call $KB_ROOT/tools/kb)";; esac

# 2. Skills: symlinks so edits in the repo apply everywhere (Omarchy does the same for its skills)
for skill in kb kb-rca; do
  run mkdir -p "$HOME/.claude/skills"
  run ln -sfn "$KB_ROOT/skills/$skill" "$HOME/.claude/skills/$skill"
  if [ -d "$HOME/.agents/skills" ]; then run ln -sfn "$KB_ROOT/skills/$skill" "$HOME/.agents/skills/$skill"; fi
done

# 3. Path-triggered rules (copies, with ~ expanded so imports resolve)
run mkdir -p "$HOME/.claude/rules"
for r in "$KB_ROOT"/rules/kb-*.md; do
  if [ "$DRY" = "--dry-run" ]; then say "would: install rule $(basename "$r")"; else
    sed "s#~/kb#$KB_ROOT#g" "$r" > "$HOME/.claude/rules/$(basename "$r")"; fi
done

# 4. Hooks merged into ~/.claude/settings.json (keeps everything else)
settings="$HOME/.claude/settings.json"
if [ "$DRY" = "--dry-run" ]; then say "would: merge hooks into $settings"; else
  [ -f "$settings" ] && cp "$settings" "$settings.bak-kb-$stamp"
  KB_ROOT="$KB_ROOT" python3 - "$settings" <<'PY'
import json, os, sys
path = sys.argv[1]; root = os.environ["KB_ROOT"]
s = json.load(open(path)) if os.path.exists(path) else {}
want = json.load(open(os.path.join(root, "tools/hooks/claude-settings-hooks.json")))
hooks = s.setdefault("hooks", {})
for event, groups in want.items():
    if event.startswith("_"):
        continue
    lst = hooks.setdefault(event, [])
    # drop earlier kb entries, then add ours
    lst[:] = [g for g in lst if not any("tools/kb hook" in h.get("command", "") for h in g.get("hooks", []))]
    for g in groups:
        g = json.loads(json.dumps(g).replace("KB_ROOT", root))
        lst.append(g)
json.dump(s, open(path, "w"), indent=2); open(path, "a").write("\n")
print("hooks merged into", path)
PY
fi

# 5. Instruction blocks between markers (CLAUDE.md for Claude, AGENTS.md for Codex)
install_block() { # file blockfile
  local f="$1" b="$2"
  if [ "$DRY" = "--dry-run" ]; then say "would: write kb block into $f"; return; fi
  mkdir -p "$(dirname "$f")"; touch "$f"; cp "$f" "$f.bak-kb-$stamp"
  python3 - "$f" "$b" "$KB_ROOT" <<'PY'
import re, sys
f, b, root = sys.argv[1:4]
block = open(b).read().replace("~/kb", root).rstrip("\n") + "\n"
text = open(f).read()
pat = re.compile(r"<!-- kb:start -->.*?<!-- kb:end -->\n?", re.S)
text = pat.sub(block, text) if pat.search(text) else (text.rstrip("\n") + "\n\n" + block if text.strip() else block)
open(f, "w").write(text)
print("kb block in", f)
PY
}
install_block "$HOME/.claude/CLAUDE.md" "$KB_ROOT/tools/claude-md-block.md"
[ -d "$HOME/.codex" ] && install_block "$HOME/.codex/AGENTS.md" "$KB_ROOT/tools/agents-md-block.md"

# 6. State dir, indexes, lint
run mkdir -p "$KB_ROOT/.state"
if [ "$DRY" != "--dry-run" ]; then python3 "$KB_ROOT/tools/kb" index; python3 "$KB_ROOT/tools/kb" lint || true; fi
say "done. Try: kb todo"
