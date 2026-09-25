# -*- coding: utf-8 -*-
"""Quick fix for compile.c and emit.c — remove Unicode box-drawing chars and close dangling comments."""
import re

def fix_file(path, marker_line_pat, close_pattern):
    """Remove lines containing box-drawing chars and close dangling comments."""
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    result = []
    i = 0
    while i < len(lines):
        line = lines[i]
        # Skip lines with box-drawing U+2550 chars (the ══ line)
        if '\u2550' in line or '\u2551' in line:
            i += 1
            continue
        # Check if previous line looks like start of a comment that will be closed next
        if close_pattern and line.strip().startswith('*/'):
            # This is the closing of a comment that was separated - skip it too
            i += 1
            continue
        result.append(line)
        i += 1

    # Now close any dangling comment at end of file
    if close_pattern:
        content = ''.join(result)
        # Find unterminated /* at end
        idx = content.rfind('/*')
        if idx >= 0:
            after = content[idx:]
            if '*/' not in after:
                # Append closing
                result = result[:-1] if result[-1].strip() == '' else result
                result.append(close_pattern)

    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines(result)
    print(f"Fixed {path}: {len(lines)} -> {len(result)} lines")

# Fix compile.c
print("Fixing compile.c...")
fix_file(
    r'f:/C-C++/LR_JS/src/engine/lr_bytecode_compile.c',
    r'COMPILER',
    '\n/* --- END COMPILE MODULE --- */\n'
)

# Fix emit.c
print("Fixing emit.c...")
fix_file(
    r'f:/C-C++/LR_JS/src/engine/lr_bytecode_emit.c',
    r'EMISSION',
    '\n/* --- END EMIT MODULE --- */\n'
)

print("Done!")
