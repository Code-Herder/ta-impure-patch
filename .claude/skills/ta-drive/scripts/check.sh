#!/usr/bin/env bash
# check.sh -- the ta-drive skill's "Maintaining this skill" rule 4 and rule 6, executable.
#
#   .claude/skills/ta-drive/scripts/check.sh        # from any checkout or worktree
#
# Exit 0 when clean. Reports:
#   * every `tagpu_<name>.<ext>` / `<name>.<ext>` lever the skill names that no source file
#     opens (the "Names that do nothing" section of references/levers.md is exempt);
#   * every `tools/...` path the skill names that does not exist;
#   * every `scenarios/<x>.json` the skill names that does not exist;
#   * dates, landing ids and history markers, which the skill must not carry;
#   * SKILL.md over 500 lines.
set -u
ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "not in a git checkout"; exit 2; }
SK="$ROOT/.claude/skills/ta-drive"
FILES=("$SK/SKILL.md" "$SK"/references/*.md)
rc=0

# the body of every file, minus the exempt dead-names section
body() {
    for f in "${FILES[@]}"; do
        awk '/^## Names that do nothing/{skip=1} /^## /{if($0 !~ /Names that do nothing/) skip=0} !skip' "$f"
    done
}

echo "== levers with no reader in tagpu/ddraw/{src,inc} or tools/tacli =="
body | grep -oE '\b(tagpu_)?[a-z0-9_]+\.(on|off|ab|cfg|txt|step|trigger|gpus|check|stress|poison)\b' \
  | sed -E 's/^tagpu_//' \
  | grep -vE '^(ddraw|impure|instance|apply|report|catalogue|tools|scenarios|user|Xorg|ta_symbols|posedump\.txt|migration\.txt)' \
  | sort -u | while read -r lev; do
    base=${lev%%.*}; ext=${lev##*.}
    # `tagpu_gui.on` is quoted once as the WRONG spelling to give `arm`; skip that literal
    [ "$base" = "tagpu_gui" ] && continue
    n=$(grep -rl -F "tagpu_${base}.${ext}" "$ROOT/tagpu/ddraw/src" "$ROOT/tagpu/ddraw/inc" "$ROOT/tools/tacli" 2>/dev/null | wc -l)
    if [ "$n" -eq 0 ]; then echo "  NO READER: $lev"; echo fail >&3; fi
done 3>"$SK/.check.tmp"
[ -s "$SK/.check.tmp" ] && rc=1; rm -f "$SK/.check.tmp"

echo "== tools/ paths that do not exist =="
cat "${FILES[@]}" | grep -oE 'tools/[A-Za-z0-9_./-]+' | sed -E 's/[.,:;)]+$//' | sort -u | while read -r p; do
    [ -e "$ROOT/$p" ] || { echo "  ABSENT: $p"; echo fail >&3; }
done 3>"$SK/.check.tmp"
[ -s "$SK/.check.tmp" ] && rc=1; rm -f "$SK/.check.tmp"

echo "== scenarios/<x>.json that do not exist =="
cat "${FILES[@]}" | grep -oE 'scenarios/[a-z0-9-]+\.json' | sort -u | while read -r p; do
    [ -e "$ROOT/$p" ] || { echo "  ABSENT: $p"; echo fail >&3; }
done 3>"$SK/.check.tmp"
[ -s "$SK/.check.tmp" ] && rc=1; rm -f "$SK/.check.tmp"

echo "== history markers (dates, landing ids, correction brackets) =="
# rule 1's own wording quotes the forbidden forms; exempt the Maintaining section
for f in "${FILES[@]}"; do
    hits=$(awk '/^## Maintaining this skill/{skip=1} !skip' "$f" \
      | grep -n -E '20[0-9]{2}-[0-9]{2}-[0-9]{2}|\bG1[0-9][a-z]?\b|\blanding [0-9]+[a-z]?(-[0-9])?\b|\[(CORRECTED|HISTORY|SUPERSEDED|MEASURED)|\bused to\b|\bsince 20[0-9]{2}')
    if [ -n "$hits" ]; then echo "$hits" | sed "s#^#  $(basename "$f"): #"; rc=1; fi
done

echo "== length =="
n=$(wc -l < "$SK/SKILL.md")
if [ "$n" -ge 500 ]; then echo "  SKILL.md is $n lines (rule 6: under 500)"; rc=1; else echo "  SKILL.md: $n lines"; fi
for f in "$SK"/references/*.md; do
    m=$(wc -l < "$f")
    if [ "$m" -gt 300 ] && ! grep -q -E '^1\. \[' "$f"; then echo "  $(basename "$f") is $m lines and has no table of contents"; rc=1; fi
done

[ "$rc" -eq 0 ] && echo "clean" || echo "FINDINGS above"
exit $rc
