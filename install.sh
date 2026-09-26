#!/usr/bin/env sh
# install.sh — build and install MyMo on Linux or macOS.
#
#   ./install.sh                 # user install into ~/.mymo (no sudo)
#   ./install.sh --system        # system install into /opt/mymo (+ /usr/local/bin/mymo)
#   ./install.sh --prefix DIR    # install into DIR
#   ./install.sh --uninstall     # remove (use the same --prefix/--system as for install)
#
# Options:
#   --no-modify-path   don't add PATH / MYMO_HOME to your shell startup file
#   --with-examples    also build and install the example extension modules
#   --skip-deps        don't check for build dependencies
#   -h, --help         show this help
#
# Layout (MYMO_HOME):
#   bin/mymo            the interpreter
#   bin/mymo-config     prints compile/link flags for extensions and embedding
#   lib/libmymo.a       the interpreter as a library (embedding)
#   lib/<name>mod.so    extension modules (.dylib on macOS), found by `use`
#   include/mymo/       headers for extensions (mymo_module.h) and embedding (mymo.h)
#   env                 sets MYMO_HOME and PATH; sourced from your shell startup files
#
# Run it from a MyMo source checkout:
#   git clone https://github.com/manoharkakumani/MyMo.git && cd MyMo && ./install.sh

set -eu

PREFIX=""
SYSTEM=0
MODIFY_PATH=1
WITH_EXAMPLES=0
CHECK_DEPS=1
UNINSTALL=0

say()  { printf '%s\n' "$*"; }
info() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# Print the header comment (everything up to the first non-comment line).
usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)          [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift ;;
        --prefix=*)        PREFIX="${1#--prefix=}" ;;
        --system)          SYSTEM=1 ;;
        --no-modify-path)  MODIFY_PATH=0 ;;
        --with-examples)   WITH_EXAMPLES=1 ;;
        --skip-deps)       CHECK_DEPS=0 ;;
        --uninstall)       UNINSTALL=1 ;;
        -h|--help)         usage; exit 0 ;;
        *)                 die "unknown option: $1 (see --help)" ;;
    esac
    shift
done

OS=$(uname -s)
case "$OS" in
    Darwin) EXT=dylib ;;
    Linux)  EXT=so ;;
    *)      die "unsupported OS '$OS' (Linux and macOS only)" ;;
esac

if [ -z "$PREFIX" ]; then
    if [ "$SYSTEM" -eq 1 ]; then PREFIX=/opt/mymo; else PREFIX="$HOME/.mymo"; fi
fi
case "$PREFIX" in /*) ;; *) PREFIX="$(pwd)/$PREFIX" ;; esac

# Use sudo only when the target isn't writable by us.
SUDO=""
needs_sudo() {
    dir="$1"
    while [ ! -d "$dir" ]; do dir=$(dirname "$dir"); done
    [ ! -w "$dir" ]
}
if needs_sudo "$PREFIX" || { [ "$SYSTEM" -eq 1 ] && needs_sudo /usr/local/bin; }; then
    command -v sudo >/dev/null 2>&1 || die "$PREFIX is not writable and sudo is not available"
    SUDO="sudo"
fi

# ---- shell startup file (PATH + MYMO_HOME) ---------------------------------

MARK_BEGIN="# >>> mymo >>>"
MARK_END="# <<< mymo <<<"

# Startup files that should load $PREFIX/env. Login shells (SSH, `su -`,
# desktop sessions) read the profile; interactive shells read the rc file
# (Ubuntu's ~/.bashrc returns early when non-interactive, so it alone is
# not enough). Printed one per line.
rc_files() {
    shell=$(basename "${SHELL:-sh}")
    case "$shell" in
        zsh)  printf '%s\n' "${ZDOTDIR:-$HOME}/.zshrc" "${ZDOTDIR:-$HOME}/.zprofile" ;;
        bash) if [ "$OS" = Darwin ]; then printf '%s\n' "$HOME/.bash_profile" "$HOME/.bashrc";
              else printf '%s\n' "$HOME/.bashrc" "$HOME/.profile"; fi ;;
        fish) printf '%s\n' "$HOME/.config/fish/conf.d/mymo.fish" ;;
        *)    printf '%s\n' "$HOME/.profile" ;;
    esac
}

# Remove a previous mymo block (between the markers) from a file.
strip_block() {
    f="$1"
    [ -f "$f" ] || return 0
    grep -q "$MARK_BEGIN" "$f" || return 0
    tmp="$f.mymo-tmp.$$"
    awk -v b="$MARK_BEGIN" -v e="$MARK_END" '
        $0 == b { skip = 1; next }
        $0 == e { skip = 0; next }
        !skip' "$f" > "$tmp" && cat "$tmp" > "$f" && rm -f "$tmp"
}

# $PREFIX/env sets MYMO_HOME and PATH (idempotently); startup files just
# source it, so a later reinstall elsewhere only has to rewrite env.
write_env() {
    tmp=$(mktemp)
    {
        printf '# Added by MyMo install.sh: sets MYMO_HOME and puts mymo on PATH.\n'
        printf 'export MYMO_HOME="%s"\n' "$PREFIX"
        printf 'case ":$PATH:" in *":$MYMO_HOME/bin:"*) ;; *) export PATH="$MYMO_HOME/bin:$PATH" ;; esac\n'
    } > "$tmp"
    $SUDO cp "$tmp" "$PREFIX/env"
    rm -f "$tmp"
}

write_block() {
    rc_files | while IFS= read -r f; do
        mkdir -p "$(dirname "$f")"
        touch "$f"
        strip_block "$f"
        if [ "$(basename "$f")" = "mymo.fish" ]; then
            {
                printf '%s\n' "$MARK_BEGIN"
                printf 'set -gx MYMO_HOME "%s"\n' "$PREFIX"
                printf 'fish_add_path -g "%s/bin"\n' "$PREFIX"
                printf '%s\n' "$MARK_END"
            } >> "$f"
        else
            {
                printf '%s\n' "$MARK_BEGIN"
                printf '[ -f "%s/env" ] && . "%s/env"\n' "$PREFIX" "$PREFIX"
                printf '%s\n' "$MARK_END"
            } >> "$f"
        fi
        say "$f"
    done
}

# ---- uninstall ---------------------------------------------------------------

if [ "$UNINSTALL" -eq 1 ]; then
    [ -x "$PREFIX/bin/mymo" ] || die "no MyMo install found in $PREFIX"
    info "Removing $PREFIX"
    $SUDO rm -rf "$PREFIX"
    if [ -L /usr/local/bin/mymo ] && [ "$(readlink /usr/local/bin/mymo)" = "$PREFIX/bin/mymo" ]; then
        info "Removing /usr/local/bin/mymo symlinks"
        $SUDO rm -f /usr/local/bin/mymo /usr/local/bin/mymo-config
    fi
    for f in "${ZDOTDIR:-$HOME}/.zshrc" "${ZDOTDIR:-$HOME}/.zprofile" "$HOME/.bashrc" \
             "$HOME/.bash_profile" "$HOME/.profile" "$HOME/.config/fish/conf.d/mymo.fish"; do
        if [ -f "$f" ] && grep -q "$MARK_BEGIN" "$f"; then
            strip_block "$f"
            info "Removed PATH/MYMO_HOME from $f"
        fi
    done
    say "MyMo uninstalled."
    exit 0
fi

# ---- source checkout -----------------------------------------------------

SRC=$(cd "$(dirname "$0")" && pwd)
[ -f "$SRC/Makefile" ] && [ -f "$SRC/vm.c" ] \
    || die "run install.sh from a MyMo source checkout (git clone https://github.com/manoharkakumani/MyMo.git)"

# ---- dependencies --------------------------------------------------------

have_header() {
    # $1: header, $2: extra cflags
    printf '#include <%s>\nint main(void){return 0;}\n' "$1" \
        | ${CC:-cc} $2 -x c -fsyntax-only - >/dev/null 2>&1
}

install_hint() {
    if [ "$OS" = Darwin ]; then
        say "  xcode-select --install        # compiler, make, libcurl, sqlite3"
    elif command -v apt-get >/dev/null 2>&1; then
        say "  sudo apt-get install build-essential libcurl4-openssl-dev libsqlite3-dev libreadline-dev"
    elif command -v dnf >/dev/null 2>&1; then
        say "  sudo dnf install gcc make libcurl-devel sqlite-devel readline-devel"
    elif command -v yum >/dev/null 2>&1; then
        say "  sudo yum install gcc make libcurl-devel sqlite-devel readline-devel"
    elif command -v pacman >/dev/null 2>&1; then
        say "  sudo pacman -S base-devel curl sqlite readline"
    elif command -v apk >/dev/null 2>&1; then
        say "  sudo apk add build-base curl-dev sqlite-dev readline-dev"
    elif command -v zypper >/dev/null 2>&1; then
        say "  sudo zypper install gcc make libcurl-devel sqlite3-devel readline-devel"
    else
        say "  install a C compiler, make, and the libcurl + sqlite3 development packages"
    fi
}

if [ "$CHECK_DEPS" -eq 1 ]; then
    info "Checking build dependencies"
    missing=""
    command -v "${CC:-cc}" >/dev/null 2>&1 || missing="$missing C-compiler"
    command -v make >/dev/null 2>&1 || missing="$missing make"
    if [ -z "$missing" ]; then
        have_header curl/curl.h "" || missing="$missing libcurl(headers)"
        have_header sqlite3.h "" || missing="$missing sqlite3(headers)"
    fi
    if [ -n "$missing" ]; then
        say "Missing:$missing"
        say "Install them with:"
        install_hint
        exit 1
    fi
    if [ "$OS" = Linux ] && ! pkg-config --exists readline 2>/dev/null; then
        warn "readline not found: the REPL will work without line editing/history"
    fi
fi

# ---- build ---------------------------------------------------------------

JOBS=$( (getconf _NPROCESSORS_ONLN || sysctl -n hw.ncpu || echo 2) 2>/dev/null | head -n 1)
BUILD_LOG=$(mktemp)
build() {
    if ! make -C "$SRC" "$@" >>"$BUILD_LOG" 2>&1; then
        say "Build failed. Last lines of the build log ($BUILD_LOG):"
        tail -n 30 "$BUILD_LOG"
        exit 1
    fi
}
info "Building MyMo (make -j$JOBS)"
build clean
build -j"$JOBS" all lib
if [ "$WITH_EXAMPLES" -eq 1 ]; then
    info "Building example extension modules"
    build ext-all
fi
rm -f "$BUILD_LOG"
"$SRC/mymo" --version >/dev/null || die "the build produced a broken binary"

# ---- install -------------------------------------------------------------

info "Installing into $PREFIX"
$SUDO mkdir -p "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/include/mymo/datatypes" "$PREFIX/include/mymo/include"
$SUDO cp "$SRC/mymo" "$PREFIX/bin/mymo"
$SUDO cp "$SRC/libmymo.a" "$PREFIX/lib/libmymo.a"
# Headers keep the source layout: include/mymo.h and include/mymo_module.h
# reach the rest through "../vm.h", "../datatypes/...".
$SUDO cp "$SRC"/*.h "$PREFIX/include/mymo/"
$SUDO cp "$SRC"/datatypes/*.h "$PREFIX/include/mymo/datatypes/"
$SUDO cp "$SRC"/include/*.h "$PREFIX/include/mymo/include/"
if [ "$WITH_EXAMPLES" -eq 1 ]; then
    $SUDO cp "$SRC"/examples/ext/*mod."$EXT" "$PREFIX/lib/"
fi

# mymo-config: flags for building extension modules and embedding hosts.
if [ "$OS" = Darwin ]; then
    EXT_LDFLAGS="-shared -fPIC -undefined dynamic_lookup"
    SYS_LIBS="-lm -lcurl -lsqlite3"
else
    EXT_LDFLAGS="-shared -fPIC"
    SYS_LIBS="-lm -lcurl -lsqlite3 -ldl"
fi
CONFIG_TMP=$(mktemp)
cat > "$CONFIG_TMP" <<EOF
#!/usr/bin/env sh
# mymo-config — compiler flags for MyMo extensions and embedding.
#   cc \$(mymo-config --cflags) -o host host.c \$(mymo-config --libs)
#   cc \$(mymo-config --cflags) \$(mymo-config --ext-ldflags) -o \$(mymo-config --moddir)/foomod.$EXT foo.c
HOME_DIR="\${MYMO_HOME:-$PREFIX}"
case "\${1:-}" in
    --cflags)      echo "-I\$HOME_DIR/include/mymo/include -I\$HOME_DIR/include/mymo" ;;
    --libs)        echo "\$HOME_DIR/lib/libmymo.a $SYS_LIBS" ;;
    --ext-ldflags) echo "$EXT_LDFLAGS" ;;
    --moddir)      echo "\$HOME_DIR/lib" ;;
    --ext)         echo "$EXT" ;;
    --prefix)      echo "\$HOME_DIR" ;;
    --version)     "\$HOME_DIR/bin/mymo" --version ;;
    *) echo "usage: mymo-config --cflags | --libs | --ext-ldflags | --moddir | --ext | --prefix | --version" >&2; exit 1 ;;
esac
EOF
$SUDO cp "$CONFIG_TMP" "$PREFIX/bin/mymo-config"
rm -f "$CONFIG_TMP"
$SUDO chmod 755 "$PREFIX/bin/mymo" "$PREFIX/bin/mymo-config"

if [ "$SYSTEM" -eq 1 ]; then
    info "Linking /usr/local/bin/mymo"
    $SUDO mkdir -p /usr/local/bin
    $SUDO ln -sf "$PREFIX/bin/mymo" /usr/local/bin/mymo
    $SUDO ln -sf "$PREFIX/bin/mymo-config" /usr/local/bin/mymo-config
fi

# ---- PATH / MYMO_HOME ----------------------------------------------------

write_env
RC_UPDATED=""
if [ "$MODIFY_PATH" -eq 1 ] && [ "$SYSTEM" -eq 0 ]; then
    RC_UPDATED=$(write_block)
    info "Added PATH and MYMO_HOME (via $PREFIX/env) to:"
    printf '%s\n' "$RC_UPDATED" | sed 's/^/      /'
fi

# ---- verify --------------------------------------------------------------

info "Verifying"
CHECK_DIR=$(mktemp -d)
cat > "$CHECK_DIR/check.my" <<'EOF'
from "json" use encode
from "mono" use get
print("ok", encode({"installed": True}))
EOF
out=$(cd "$CHECK_DIR" && MYMO_HOME="$PREFIX" MYMO_NOCACHE=1 "$PREFIX/bin/mymo" check.my 2>&1) \
    || { rm -rf "$CHECK_DIR"; die "installed interpreter failed: $out"; }
rm -rf "$CHECK_DIR"
[ "$out" = 'ok {"installed":true}' ] || die "unexpected output from installed interpreter: $out"

say ""
say "$("$PREFIX/bin/mymo" --version) installed in $PREFIX"
if [ "$SYSTEM" -eq 1 ]; then
    say "Run:  mymo --version"
elif [ -n "$RC_UPDATED" ]; then
    say "Open a new terminal, or run:  . \"$PREFIX/env\""
else
    say "To use it, add this line to your shell startup file:"
    say "  . \"$PREFIX/env\""
fi
say "Extension modules go in: $PREFIX/lib   (flags: mymo-config --cflags | --libs | --ext-ldflags)"
