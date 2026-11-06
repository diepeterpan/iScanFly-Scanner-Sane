#!/bin/bash
set -e

# ==============================================================================
# verify.sh - Static analysis, code formatting check, and script linting
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Check if running inside container or if tools are missing locally
MISSING_TOOLS=0
for tool in cppcheck clang-format shellcheck; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        MISSING_TOOLS=1
        break
    fi
done

# If tools are missing on host and not already inside a container, delegate to Docker
if [[ "$MISSING_TOOLS" -eq 1 && ! -f /.dockerenv ]]; then
    echo "Notice: Linters not detected locally on host. Delegating to Docker container..."
    exec docker run --rm -v "$REPO_ROOT":/build -u "$(id -u):$(id -g)" iscanfly-builder ./scripts/verify.sh "$@"
fi

DO_FIX=0
if [[ "$1" == "--fix" ]]; then
    DO_FIX=1
fi

echo "=== Running Code Verification & Best Practice Checks ==="

ERRORS=0

# 1. ShellCheck
echo ""
echo "[1/4] Running ShellCheck on shell scripts..."
SHELL_SCRIPTS=()
while IFS= read -r f; do
    SHELL_SCRIPTS+=("$f")
done < <(find "$REPO_ROOT/scripts" "$REPO_ROOT/tools" "$REPO_ROOT/docker" -name "*.sh")

if shellcheck -x "${SHELL_SCRIPTS[@]}"; then
    echo "✓ ShellCheck passed cleanly."
else
    echo "✗ ShellCheck found issues."
    ERRORS=$((ERRORS + 1))
fi

# 2. Clang-Format
echo ""
echo "[2/4] Checking C code formatting with clang-format..."
C_FILES=()
while IFS= read -r f; do
    C_FILES+=("$f")
done < <(find "$REPO_ROOT/src" "$REPO_ROOT/include" "$REPO_ROOT/tools" -name "*.[ch]")

if [[ "$DO_FIX" -eq 1 ]]; then
    echo "Formatting files with clang-format -i..."
    clang-format -i "${C_FILES[@]}"
    echo "✓ Formatting applied."
else
    FORMAT_DIFF=0
    for file in "${C_FILES[@]}"; do
        if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
            echo "Formatting issue in: $file (run ./scripts/verify.sh --fix to format)"
            FORMAT_DIFF=1
        fi
    done
    if [[ "$FORMAT_DIFF" -eq 0 ]]; then
        echo "✓ Clang-format check passed cleanly."
    else
        echo "✗ Clang-format identified styling mismatches."
        # Don't fail build on formatting unless requested
    fi
fi

# 3. Cppcheck static analysis
echo ""
echo "[3/4] Running Cppcheck static analysis..."
if cppcheck --enable=warning,performance,portability \
            --error-exitcode=1 \
            --suppress=missingIncludeSystem \
            --suppress=normalCheckLevelMaxBranches \
            --inline-suppr \
            -I "$REPO_ROOT/include" \
            "$REPO_ROOT/src" "$REPO_ROOT/tools/scanner.c"; then
    echo "✓ Cppcheck static analysis passed."
else
    echo "✗ Cppcheck reported warnings or errors."
    ERRORS=$((ERRORS + 1))
fi

# 4. Strict Compiler Build Check (-Wall -Wextra)
echo ""
echo "[4/4] Strict compiler build check..."
if gcc -Wall -Wextra -Werror -fPIC -I"$REPO_ROOT/include" -c "$REPO_ROOT/src/iscanfly-sane.c" -o /tmp/test_sane.o; then
    rm -f /tmp/test_sane.o
    echo "✓ SANE backend compiled cleanly with -Wall -Wextra -Werror."
else
    echo "✗ SANE backend compiler check failed."
    ERRORS=$((ERRORS + 1))
fi

echo ""
if [[ "$ERRORS" -eq 0 ]]; then
    echo "==============================================="
    echo "✓ ALL BEST PRACTICE VERIFICATION CHECKS PASSED!"
    echo "==============================================="
    exit 0
else
    echo "==============================================="
    echo "✗ VERIFICATION FAILED ($ERRORS error(s) found)"
    echo "==============================================="
    exit 1
fi
