#!/bin/bash
set -e

# ==============================================================================
# distribute.sh - Transfer iScanFly SANE driver bundle to a remote machine via SCP
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST_DIR="$REPO_ROOT/dist"

PORT="22"
IDENTITY_FILE=""
REMOTE_TARGET_DIR="iscanfly-dist"
DO_INSTALL=0
REMOTE_DEST=""

usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS] <[user@]remote_host>

Copies the compiled 'dist/' directory to a remote machine using standard scp.

Options:
  -P, --port <port>        SSH port (default: 22)
  -i, --identity <key>     SSH private key path
  -d, --dir <path>         Destination directory on remote host (default: ~/iscanfly-dist)
      --install            Automatically execute sudo ./install.sh on the remote host
  -h, --help               Display this help message

Examples:
  $(basename "$0") pi@192.168.1.50
  $(basename "$0") -P 2222 -i ~/.ssh/id_ed25519 --install user@scanserver.local
  $(basename "$0") --install root@192.168.18.10
EOF
    exit 1
}

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -P|--port)
            PORT="$2"
            shift 2
            ;;
        -i|--identity)
            IDENTITY_FILE="$2"
            shift 2
            ;;
        -d|--dir)
            REMOTE_TARGET_DIR="$2"
            shift 2
            ;;
        --install)
            DO_INSTALL=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            if [[ -z "$REMOTE_DEST" ]]; then
                REMOTE_DEST="$1"
                shift
            else
                echo "Error: Unexpected argument '$1'" >&2
                usage
            fi
            ;;
    esac
done

if [[ -z "$REMOTE_DEST" ]]; then
    echo "Error: Remote destination (<[user@]remote_host>) is required." >&2
    usage
fi

# Verify dist directory
if [[ ! -d "$DIST_DIR" || ! -f "$DIST_DIR/libsane-iscanfly.so.1.0.0" ]]; then
    echo "Notice: 'dist/' directory not found or incomplete. Building via Docker..."
    docker run --rm -v "$REPO_ROOT":/build -u "$(id -u):$(id -g)" iscanfly-builder make dist
fi

echo "=== Distributing iScanFly SANE driver to $REMOTE_DEST ==="

# Build SSH and SCP command options
SSH_OPTS=(-p "$PORT")
SCP_OPTS=(-P "$PORT")

if [[ -n "$IDENTITY_FILE" ]]; then
    SSH_OPTS+=(-i "$IDENTITY_FILE")
    SCP_OPTS+=(-i "$IDENTITY_FILE")
fi

# 1. Create target directory on remote host
echo "Creating remote directory '$REMOTE_TARGET_DIR'..."
# shellcheck disable=SC2029
ssh "${SSH_OPTS[@]}" "$REMOTE_DEST" "mkdir -p $REMOTE_TARGET_DIR"

# 2. Transfer dist files using scp -r
echo "Copying files to $REMOTE_DEST:$REMOTE_TARGET_DIR/ ..."
scp -r "${SCP_OPTS[@]}" "$DIST_DIR"/* "$REMOTE_DEST:$REMOTE_TARGET_DIR/"

# 3. Ensure install.sh is executable on remote host
# shellcheck disable=SC2029
ssh "${SSH_OPTS[@]}" "$REMOTE_DEST" "chmod +x $REMOTE_TARGET_DIR/install.sh"

echo "Files successfully copied to $REMOTE_DEST:$REMOTE_TARGET_DIR"

# 4. Optional remote install
if [[ "$DO_INSTALL" -eq 1 ]]; then
    echo ""
    echo "=== Running remote installation on $REMOTE_DEST ==="
    ssh -t "${SSH_OPTS[@]}" "$REMOTE_DEST" "cd $REMOTE_TARGET_DIR && sudo ./install.sh"
    echo "=== Remote installation complete! ==="
else
    echo ""
    echo "To complete installation on the remote machine, log in and run:"
    echo "    ssh ${SSH_OPTS[*]} $REMOTE_DEST 'cd $REMOTE_TARGET_DIR && sudo ./install.sh'"
fi

