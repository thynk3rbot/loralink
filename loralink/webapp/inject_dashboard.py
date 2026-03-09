import os
import re

filepath = r"c:\Users\spw1\OneDrive\Documents\Code\Antigravity Repository\antigravity\tools\webapp\static\mqtt_dashboard.html"

with open(filepath, "r", encoding="utf-8") as f:
    text = f.read()

css_rule = ".dbg-id { position: absolute; right: 8px; top: 8px; font-size: 10px; color: #555; opacity: 0.6; pointer-events: none; font-family: monospace; letter-spacing: 0.5px; z-index: 100; }"
if "dbg-id" not in text and "</style>" in text:
    text = text.replace("</style>", f"  {css_rule}\n</style>")

counter = 41


def rep(m):
    global counter
    tag = m.group(1)
    if "dbg-id" in tag:
        return tag
    res = tag + f'<span class="dbg-id">FA-{counter:03d}</span>'
    counter += 1
    return res


pattern = re.compile(
    r'(<(?:div|section)[^>]*class="[^"]*\b(?:panel|node-card)\b[^"]*"[^>]*>)'
)
new_text = pattern.sub(rep, text)

print(f"Replaced {counter - 41} elements")
with open(filepath, "w", encoding="utf-8") as f:
    f.write(new_text)
