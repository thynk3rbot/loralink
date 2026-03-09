import os
import re

base_dir = r"c:\Users\spw1\OneDrive\Documents\Code\Antigravity Repository\antigravity\tools\webapp\static"
files = ["index.html", "mqtt_dashboard.html", "scheduler.html"]

counter = 1

def replacer(m):
    global counter
    tag_content = m.group(1)
    if 'dbg-id' in tag_content:
        # Already has an ID, but we only appended it AFTER the tag in a span
        pass

    # We want to match the whole opening tag and append the span Right after it
    # m.group(1) is the whole <div class="card" ...>
    injected_span = f'<span class="dbg-id">FA-{counter:03d}</span>'
    counter += 1
    return tag_content + injected_span

# Match <div ... class="... card ..." ...> or <section ...>
# We can use a regex that safely grabs the opening tag of elements with specific classes.
pattern = re.compile(r'(<(?:div|section)[^>]*class="[^"]*\b(?:card|stat-card|glass-panel|widget|modal-content)\b[^"]*"[^>]*>)')

for filename in files:
    filepath = os.path.join(base_dir, filename)
    if not os.path.exists(filepath): continue

    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    # Inject CSS if not present
    css_rule = ".dbg-id { position: absolute; right: 8px; top: 8px; font-size: 10px; color: #555; opacity: 0.6; pointer-events: none; font-family: monospace; letter-spacing: 0.5px; z-index: 100; }"
    if "dbg-id" not in content and "</style>" in content:
        content = content.replace("</style>", f"  {css_rule}\n</style>")

    # Also need to make sure positions are relative for the cards if not already (most are)
    # The CSS class already has position:absolute so the parent needs position:relative
    # It's usually fine.

    new_content = pattern.sub(replacer, content)

    if new_content != content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print(f"Updated {filename} with IDs up to FA-{counter-1}")
    else:
        print(f"No changes made to {filename}")
