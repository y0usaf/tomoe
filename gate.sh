#!/usr/bin/env bash
set -uo pipefail

PROG=tomoe-gate
SYSTEM=${GATE_SYSTEM:-x86_64-linux}
FINIX=${GATE_FINIX:-$HOME/finix}
HOST=${GATE_HOST:-$(hostname)}
ACCOUNT=${USER:-$(id -un)}
OUTPUT=${GATE_OUTPUT:-DP-4}
MODE=${GATE_MODE:-5120x1440@240}
WINDOWS=${GATE_WINDOWS:-4}
SWITCHES=${GATE_SWITCHES:-48}
PALETTE=${GATE_PALETTE:-$HOME/.cache/theme/gtk.css}
LOCK=${TOMOE_LOCK:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/agent/tomoe-vm.lock}
PENDING=${TOMOE_PENDING:-$(dirname "$LOCK")/tomoe-vm.pending}

say() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() {
	printf '%s: FAIL: %s\n' "$PROG" "$*" >&2
	[[ -n ${DIR:-} ]] && printf '%s: kept %s\n' "$PROG" "$DIR" >&2
	KEEP=1
	exit 1
}

usage() {
	cat <<'EOF'
usage: gate.sh [REV]

Builds REV (default HEAD) the way finix would pin it, with the config finix
generates against that very build, and runs the pair on a private headless
instance with its own socket, runtime directory and home: one output at
5120x1440@240, real test windows, dark then light, every rail theme in turn,
rapid focus switches and moves with windows closing and opening mid-animation,
a reload, and a config made for another build. It fails on any extension
failure, duplicate timer, lost focus, unplaced window, or a mismatched config
that loads. The session has no network, so services the config starts
cannot reach a device. Nothing reaches the running desktop.

  GATE_FINIX    finix checkout whose HEAD is used (default ~/finix)
  GATE_HOST     finix host (default this machine's hostname)
  GATE_OUTPUT   output name the config arranges (default DP-4)
  GATE_MODE     WxH@HZ of that output (default 5120x1440@240)
  GATE_PALETTE  palette the theme reads, copied in (default ~/.cache/theme/gtk.css)
EOF
}

[[ ${1:-} == -h || ${1:-} == --help ]] && {
	usage
	exit 0
}

REPO=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the tomoe checkout"
REV=$(git -C "$REPO" rev-parse --verify "${1:-HEAD}^{commit}" 2>/dev/null) || die "no commit ${1:-HEAD}"
[[ $MODE =~ ^([0-9]+)x([0-9]+)@([0-9]+)$ ]] || die "GATE_MODE must be WxH@HZ"
WIDTH=${BASH_REMATCH[1]} HEIGHT=${BASH_REMATCH[2]} HZ=${BASH_REMATCH[3]}
[[ -r $PALETTE ]] || die "no palette at $PALETTE"

lease() {
	local waited=0
	while [[ -n $(ls -A "$PENDING" 2>/dev/null) ]] && ((waited < 600)); do
		sleep 2
		waited=$((waited + 2))
	done
	mkdir -p "$(dirname "$LOCK")"
	exec 9>"$LOCK" && flock -s 9
}

FINIXREV=$(git -C "$FINIX" rev-parse --verify HEAD 2>/dev/null) || die "no finix checkout at $FINIX"
say "building $REV with the config finix $FINIXREV generates for $HOST"
lease || die "cannot take $LOCK"
SOURCE="git+file://$REPO?rev=$REV"
CONFIG="git+file://$FINIX?rev=$FINIXREV"
PKG=$(nix build --no-link --print-out-paths --override-input tomoe "$SOURCE" \
	"$CONFIG#packages.$SYSTEM.tomoe") || die "finix cannot build tomoe $REV"
INIT=$(nix build --no-link --print-out-paths --override-input tomoe "$SOURCE" \
	"$CONFIG#nixosConfigurations.$HOST.config.manzil.users.$ACCOUNT.files.\".config/tomoe/init.lisp\".source") ||
	die "finix cannot generate the config"
CLIENTS_OUT=$(nix build --no-link --print-out-paths "$SOURCE#legacyPackages.$SYSTEM.client") ||
	die "cannot build the test client"
CLIENT=$CLIENTS_OUT/bin/tomoe-test-client
KEYS=$CLIENTS_OUT/bin/tomoe-pointer-script
exec 9>&-
grep -qF "(require-tomoe \"$PKG\")" "$INIT" || die "$INIT does not require $PKG"
say "pair $PKG + $INIT"

DIR=$(mktemp -d "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/tomoe-gate.XXXXXX")
KEEP=0
SOCKET=gate-$$
mkdir -m 700 "$DIR/run"
mkdir -p "$DIR/home/.cache/theme" "$DIR/home/.config"
cp -rL "$HOME/.config/fontconfig" "$DIR/home/.config/" 2>/dev/null
sed "s#$HOME/#$DIR/home/#g" "$INIT" >"$DIR/init.lisp"
cp "$INIT" "$DIR/paired.lisp"
sed "s#$HOME/#$DIR/home/#g" "$PALETTE" >"$DIR/first.css"
BG=$(awk '$1 == "@define-color" && $2 == "bg" {print $3}' "$DIR/first.css" | tr -d ';')
FG=$(awk '$1 == "@define-color" && $2 == "fg" {print $3}' "$DIR/first.css" | tr -d ';')
[[ -n $BG && -n $FG && $BG != "$FG" ]] || die "$PALETTE defines no distinct bg and fg"
sed "s/$BG/@BG@/g; s/$FG/$BG/g; s/@BG@/$FG/g" "$DIR/first.css" >"$DIR/second.css"
cp "$DIR/first.css" "${PALETTE/#$HOME\//$DIR/home/}"
ENV=(env -i "HOME=$DIR/home" "USER=$ACCOUNT" "PATH=$PATH" "XDG_RUNTIME_DIR=$DIR/run" "LANG=${LANG:-C.UTF-8}")
SERVER=""
CLIENTS=()

cleanup() {
	if [[ -n $SERVER ]]; then
		"${ENV[@]}" "$PKG/bin/tomoe" --socket "$SOCKET" quit >/dev/null 2>&1
		sleep 0.5
		kill -TERM -- "-$SERVER" 2>/dev/null
	fi
	for pid in "${CLIENTS[@]}"; do kill -TERM -- "-$pid" 2>/dev/null; done
	((KEEP)) || rm -rf "$DIR"
}
trap cleanup EXIT

ctl() { "${ENV[@]}" "$PKG/bin/tomoe" --socket "$SOCKET" "$@"; }

"${ENV[@]}" unshare --user --map-current-user --net setsid "$PKG/bin/tomoe" --socket "$SOCKET" --backend headless --no-watch \
	--config "$DIR/init.lisp" >"$DIR/log" 2>&1 </dev/null &
SERVER=$!
for _ in $(seq 300); do ctl inspect >/dev/null 2>&1 && break; sleep 0.1; done
ctl inspect >"$DIR/inspect" 2>&1 || die "the session never answered"
grep -q ':CONFIG-ERROR NIL' "$DIR/inspect" || die "the paired config did not load: $(grep -o ':CONFIG-ERROR ([^)]*' "$DIR/inspect" | head -1)"

ctl command patch name=gate-screen "forms=(in-package #:tomoe-user)
(define-extension \"gate-screen\" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values state (list (virtual-output \"$OUTPUT\" :mode (list $WIDTH $HEIGHT $HZ))
                      (configure-output \"HEADLESS-1\" :disabled t)) nil))" >/dev/null ||
	die "cannot give the session its $MODE output"
for _ in $(seq 100); do
	ctl msg outputs 2>/dev/null | grep -q "\"w\": $WIDTH" && ! ctl msg outputs | grep -q '"HEADLESS-1"' && break
	sleep 0.1
done
ctl msg outputs | grep -q "\"w\": $WIDTH" || die "the session has no $MODE output"

serial=0
open_window() {
	serial=$((serial + 1))
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" setsid "$CLIENT" --seconds 3600 --app-id "gate-$serial" \
		--title "gate-$serial" >>"$DIR/clients" 2>&1 </dev/null &
	CLIENTS+=($!)
}

windows() { ctl msg windows | grep -c '"app_id"'; }

settle() {
	local want=$1
	for _ in $(seq 100); do
		[[ $(windows) == "$want" ]] && break
		sleep 0.05
	done
	sleep 0.6
}

extensions() { grep -o ':DISPATCHES [0-9]*' "$DIR/inspect" | wc -l; }

check() {
	local label=$1 want=$2
	ctl inspect >"$DIR/inspect" 2>&1 || die "$label: the session stopped answering"
	local failed dup count focused unplaced visible
	failed=$(grep -E '^Extension |Duplicate effect|did not settle' "$DIR/log" | sort | uniq -c)
	[[ -z $failed ]] || die "$label: extensions failed:
$failed"
	dup=$(grep -o '(:OWNER "[^"]*" :NAME :[^ )]*' "$DIR/inspect" | sort | uniq -d)
	[[ -z $dup ]] || die "$label: duplicate timers: $dup"
	ctl msg windows >"$DIR/windows" || die "$label: no window list"
	count=$(grep -c '"app_id"' "$DIR/windows")
	[[ $count == "$want" ]] || die "$label: $count windows, expected $want"
	focused=$(grep -c '"focused": true' "$DIR/windows")
	((want == 0 || focused == 1)) || die "$label: $focused focused windows among $count"
	unplaced=$(grep -cE '"(w|h)": 0,?$' "$DIR/windows")
	((unplaced == 0)) || die "$label: $unplaced windows without a size"
	visible=$(awk -v W="$WIDTH" -v H="$HEIGHT" '
		/"x":/ {gsub(/[^-0-9]/, ""); x = $0} /"y":/ {gsub(/[^-0-9]/, ""); y = $0}
		/"w":/ {gsub(/[^-0-9]/, ""); w = $0} /"h":/ {gsub(/[^-0-9]/, ""); h = $0}
		/"focused": true/ {print ((x + 0 < W + 0 && y + 0 < H + 0 && x + w > 0 && y + h > 0) ? 1 : 0)}' "$DIR/windows")
	((want == 0)) || [[ $visible == 1 ]] || die "$label: the focused window is off the $MODE output"
}

mode_of() { grep -o ':THEME-MODE :[A-Z]*' "$DIR/inspect" | tail -1 | cut -d: -f3; }
theme_of() { grep -o ':RAIL-THEME :[A-Z-]*' "$DIR/inspect" | tail -1 | cut -d: -f3; }
focused() { ctl msg windows | awk '/^    "id":/ {gsub(/[^0-9]/, ""); id = $0} /"focused": true/ {print id}'; }

run() {
	ctl command "$@" >"$DIR/command" 2>&1 || die "command $*: $(cat "$DIR/command")"
}

hammer() {
	local label=$1 i
	for ((i = 0; i < SWITCHES; i++)); do
		case $((i % 8)) in
		0) run next ;;
		1) run focus-left ;;
		2) run previous ;;
		3) run move-down ;;
		4) run focus-right ;;
		5) run to-left ;;
		6) run close-window && open_window ;;
		7) run to-right ;;
		esac
	done
	settle "$WINDOWS"
	check "$label" "$WINDOWS"
	steer "$label"
}

await_focus() {
	local want=$1 now=""
	for _ in $(seq 60); do
		now=$(focused)
		[[ $now == "$want" ]] && return 0
		sleep 0.05
	done
	return 1
}

steer() {
	local label=$1 start there back=previous
	start=$(focused)
	[[ -n $start ]] || die "$label: nothing focused"
	run next
	sleep 0.3
	there=$(focused)
	if [[ $there == "$start" ]]; then
		run previous
		sleep 0.3
		there=$(focused)
		back=next
	fi
	[[ -n $there && $there != "$start" ]] || die "$label: next and previous left window $start focused"
	run "$back"
	await_focus "$start" || die "$label: $back after a step from $start left $(focused) focused"
}

chord() {
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" "key 56 down" "key 42 down" "key 20 down" sync \
		"key 20 up" "key 42 up" "key 56 up" sync >"$DIR/keys" 2>&1 || die "cannot type Alt+Shift+T: $(cat "$DIR/keys")"
}

THEMES=$(grep -o '(defparameter +rail-themes+ (list .*' "$DIR/init.lisp" | grep -o ' :[a-z-]* (list\* :shader' |
	cut -d' ' -f2 | tr 'a-z' 'A-Z' | sed 's/^://' | sort | tr '\n' ' ')
[[ -n $THEMES ]] || die "the config declares no rail themes"
grep -qE ':MODIFIERS 9 :CODE [0-9]+ :KEYSYM "t" :OWNER "[^"]*" :CALL \("rail-theme"\)' "$DIR/inspect" ||
	die "Alt+Shift+T is not bound to rail-theme"
for ((i = 0; i < WINDOWS; i++)); do open_window; done
settle "$WINDOWS"
check "open $WINDOWS windows" "$WINDOWS"

for palette in first second; do
	before=$(mode_of)
	cp "$DIR/$palette.css" "$DIR/palette.new" && mv "$DIR/palette.new" "${PALETTE/#$HOME\//$DIR/home/}"
	for _ in $(seq 100); do
		ctl inspect >"$DIR/inspect"
		[[ $palette == first || $(mode_of) != "$before" ]] && break
		sleep 0.05
	done
	mode=$(mode_of)
	[[ -n $mode && ($palette == first || $mode != "$before") ]] || die "the theme did not follow the $palette palette"
	first=$(theme_of) seen=""
	[[ -n $first ]] || die "$mode: the rail names no theme"
	while :; do
		theme=$(theme_of)
		[[ " $seen " == *" $theme "* ]] && die "$mode: Alt+Shift+T came back to $theme before $first"
		say "$mode $theme"
		hammer "$mode $theme"
		seen="$seen $theme"
		chord
		for _ in $(seq 60); do
			ctl inspect >"$DIR/inspect"
			[[ $(theme_of) != "$theme" ]] && break
			sleep 0.05
		done
		[[ $(theme_of) != "$theme" ]] || die "$mode: Alt+Shift+T left the rail on $theme"
		[[ $(theme_of) == "$first" ]] && break
	done
	[[ $(tr ' ' '\n' <<<"$seen" | sed '/^$/d' | sort | tr '\n' ' ') == "$THEMES" ]] ||
		die "$mode: Alt+Shift+T visited$seen, the config has $THEMES"
	say "$mode themes:$seen"
done

ctl reload >"$DIR/reload" 2>&1 || die "reload failed: $(cat "$DIR/reload")"
settle "$WINDOWS"
check "reload" "$WINDOWS"

mounted=$(extensions)
cp "$DIR/init.lisp" "$DIR/good.lisp"
sed -i 's#(require-tomoe "[^"]*")#(require-tomoe "/nix/store/00000000000000000000000000000000-tomoe-other")#' "$DIR/init.lisp"
if ctl reload >"$DIR/reload" 2>&1; then die "a config for another build loaded"; fi
grep -q 'is for tomoe /nix/store/00000000000000000000000000000000-tomoe-other' "$DIR/reload" ||
	die "a config for another build failed without naming it: $(cat "$DIR/reload")"
settle "$WINDOWS"
check "mismatched config" "$WINDOWS"
[[ $(extensions) == "$mounted" ]] || die "the refused config changed the mounted extensions"
cp "$DIR/good.lisp" "$DIR/init.lisp"
ctl reload >"$DIR/reload" 2>&1 || die "reload after the refusal failed: $(cat "$DIR/reload")"
hammer "after reload"

say "PASS $REV $PKG"
