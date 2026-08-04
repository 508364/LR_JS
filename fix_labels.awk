# Extract DOP mappings and generate sed script
/DOP\(BC_/ {
    gsub(/^[ \t]+/, "")
    sub(/DOP\(/, "")
    sub(/\).*/, "")
    split($0, a, /,[ \t]*/)
    op  = a[1]   # e.g. BC_STOP
    lbl = a[2]   # e.g. stop
    # Generate sed: s/\bcase BC_STOP:/BC_CASE(stop, BC_STOP):/g
    printf "s/\\bcase %s:/BC_CASE(%s, %s):/g\n", op, lbl, op
}
