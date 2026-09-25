# -*- coding: utf-8 -*-
"""Fix orphan section headers left by the split process in emit.c and compile.c."""
import re

def fix_orphan_section_headers(path):
    """Remove orphaned section header lines that survived the split."""
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Remove orphan section header fragments like "   COMPILER STATE\n   =======\n"
    # Pattern: whitespace + WORDS + newline + whitespace + ===+ + */
    content = re.sub(
        r'\n\s+[A-Z][A-Z _]+\n\s+[=|+\-]+\s*\*/',
        '\n',
        content
    )
    # Also catch: "   COMPILER\n   ===" without closing */
    content = re.sub(
        r'\n\s+[A-Z][A-Z _]+\n\s+[=|+\-]+',
        '\n',
        content
    )
    # Remove trailing orphan: "   EMISSION (end of emit module)" on its own line
    content = re.sub(
        r'\n\s+[A-Z_]+ \(end of [a-z]+ module\)\n\s*[=|+\-]+\s*\*/',
        '\n',
        content
    )

    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    print(f"Fixed {path}")

fix_orphan_section_headers(r'f:/C-C++/LR_JS/src/engine/lr_bytecode_emit.c')
fix_orphan_section_headers(r'f:/C-C++/LR_JS/src/engine/lr_bytecode_compile.c')
print("Done!")
