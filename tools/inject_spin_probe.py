"""TEMP diagnostic: inject a per-site cap before each BACKWARD `goto L_X;` in a
recompiled RSP ucode, so the spinning goto prints its own source line. Run on
musyx_audio_recompiled.c, build, run; remove by regenerating the file.

Injects a brace-wrapped statement on its OWN line right before the goto, so it
works whether the goto is inside `if{...}` or bare. No newline in the C string
(avoids shell/heredoc escaping issues)."""
import re, sys

p = sys.argv[1]
lines = open(p, encoding="utf-8").read().split("\n")
labeldef = {}
for i, l in enumerate(lines):
    m = re.match(r"^(L_[0-9A-Fa-f]+):\s*$", l)
    if m:
        labeldef[m.group(1)] = i

out = []
inj = 0
for i, l in enumerate(lines):
    m = re.match(r"^(\s*)goto (L_[0-9A-Fa-f]+);\s*$", l)
    if m and m.group(2) in labeldef and labeldef[m.group(2)] < i:
        ind, tgt = m.group(1), m.group(2)
        probe = (ind + "{ static long _c = 0; if (++_c > 800000L) { "
                 'fprintf(stderr, "[spin] ' + tgt + ' C-line %d  r1=0x%X r2=0x%X r5=0x%X r6=0x%X r26=0x%X r29=0x%X ", '
                 "__LINE__, r1, r2, r5, r6, r26, r29); "
                 "fflush(stderr); return RspExitReason::Broke; } }  /* spin-probe */")
        out.append(probe)
        inj += 1
    out.append(l)
open(p, "w", encoding="utf-8").write("\n".join(out))
print(f"labels={len(labeldef)} backward-goto probes injected={inj}")
