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
SOAK=${GATE_SOAK:-0}
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
a reload, and a config whose middle form calls an undefined function. It fails
on any extension failure, duplicate timer, lost focus, unplaced window, or a
broken config that loads or disturbs the running one: its windows, binds
(typed Super+Tab and Alt+Shift+T), rail theme, surfaces and picture must
hold. The session has no network, so services the config starts cannot reach a device.
Nothing reaches the running desktop.

  GATE_FINIX    finix checkout (default ~/finix)
  GATE_FINIX_REV  finix revision whose config is used (default its HEAD)
  GATE_HOST     finix host (default this machine's hostname)
  GATE_OUTPUT   output name the config arranges (default DP-4)
  GATE_MODE     WxH@HZ of that output (default 5120x1440@240)
  GATE_PALETTE  palette the theme reads, copied in (default ~/.cache/theme/gtk.css)
  GATE_SOAK     after the run, keep cycling palettes, themes, focus storms and
                reloads for this many seconds (default 0); a soak fails the same
                way, and on the compositor exiting or logging a fatal error

It refuses to start while DISPLAY, WAYLAND_DISPLAY, WAYLAND_SOCKET or
TOMOE_SOCKET is set or a fake kernel input device exists (see tools/isolate).
EOF
}

[[ ${1:-} == -h || ${1:-} == --help ]] && {
	usage
	exit 0
}

if [[ -n ${TOMOE_ISOLATED:-} ]]; then
	source "$TOMOE_ISOLATED/gate.state"
else
	TOOLS=$(dirname "$(realpath "${BASH_SOURCE[0]}")")/tools
	ISOLATE=$TOOLS/isolate
	# shellcheck source=tools/lease.sh
	. "$TOOLS/lease.sh"
	"$ISOLATE" --check || exit 2
	REPO=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the tomoe checkout"
	REV=$(git -C "$REPO" rev-parse --verify "${1:-HEAD}^{commit}" 2>/dev/null) || die "no commit ${1:-HEAD}"
	[[ $SOAK =~ ^[0-9]+$ ]] || die "GATE_SOAK must be a number of seconds"
	[[ $MODE =~ ^([0-9]+)x([0-9]+)@([0-9]+)$ ]] || die "GATE_MODE must be WxH@HZ"
	WIDTH=${BASH_REMATCH[1]} HEIGHT=${BASH_REMATCH[2]} HZ=${BASH_REMATCH[3]}
	[[ -r $PALETTE ]] || die "no palette at $PALETTE"

	lease() {
		local waited=0
		while [[ -n $(ls -A "$PENDING" 2>/dev/null) ]] && ((waited < 600)); do
			sleep 2
			waited=$((waited + 2))
		done
		lease_take "$LOCK" -s 0
	}

	FINIXREV=$(git -C "$FINIX" rev-parse --verify "${GATE_FINIX_REV:-HEAD}^{commit}" 2>/dev/null) ||
		die "no finix ${GATE_FINIX_REV:-HEAD} at $FINIX"
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
	lease_release
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
	THEME_FILE=${PALETTE/#$HOME\//$DIR/home/}
	cp "$DIR/first.css" "$THEME_FILE"
	declare -p REV FINIXREV PKG INIT CLIENT KEYS SOCKET WIDTH HEIGHT HZ OUTPUT MODE WINDOWS SWITCHES SOAK \
		ACCOUNT PALETTE THEME_FILE DIR >"$DIR/gate.state"
	cp "${BASH_SOURCE[0]}" "$DIR/gate.sh"
	"$ISOLATE" "$DIR" bash "$DIR/gate.sh"
	rc=$?
	if ((rc == 0)); then rm -rf "$DIR"; else say "kept $DIR"; fi
	exit "$rc"
fi
KEEP=0
ENV=(env)
SERVER=""
CLIENTS=()

cleanup() {
	if [[ -n $SERVER ]]; then
		"${ENV[@]}" "$PKG/bin/tomoe" --socket "$SOCKET" quit >/dev/null 2>&1
		sleep 0.5
		kill -TERM -- "-$SERVER" 2>/dev/null
	fi
	for pid in "${CLIENTS[@]}"; do kill -TERM -- "-$pid" 2>/dev/null; done
}
trap cleanup EXIT

ctl() { "${ENV[@]}" "$PKG/bin/tomoe" --socket "$SOCKET" "$@"; }

setsid "$PKG/bin/tomoe" --socket "$SOCKET" --backend headless --no-watch \
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
	local label=$1 want=$2 fatal
	kill -0 "$SERVER" 2>/dev/null || die "$label: the compositor exited:
$(tail -5 "$DIR/log")"
	fatal=$(grep -E 'stopping the compositor|native event loop failed|Segmentation fault|Aborted|core dumped' "$DIR/log" | head -3)
	[[ -z $fatal ]] || die "$label: the compositor logged a fatal error:
$fatal"
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

cycle() {
	local palette before same mode first seen theme
	for palette in first second; do
		before=$(mode_of) same=false
		[[ $(<"$DIR/$palette.css") == "$(<"$THEME_FILE")" ]] && same=true
		cp "$DIR/$palette.css" "$DIR/palette.new" && mv "$DIR/palette.new" "$THEME_FILE"
		for _ in $(seq 100); do
			ctl inspect >"$DIR/inspect"
			{ $same || [[ $(mode_of) != "$before" ]]; } && break
			sleep 0.05
		done
		mode=$(mode_of)
		[[ -n $mode ]] && { $same || [[ $mode != "$before" ]]; } || die "the theme did not follow the $palette palette"
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
}
cycle

ctl reload >"$DIR/reload" 2>&1 || die "reload failed: $(cat "$DIR/reload")"
settle "$WINDOWS"
check "reload" "$WINDOWS"

binds() { grep -oE ':MODIFIERS [0-9]+ :CODE [0-9]+ :KEYSYM "[^"]*" :OWNER "[^"]*" :CALL \([^)]*\)' "$DIR/inspect" | sort; }
shot() {
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" grim -t ppm "$1" 2>"$DIR/grim" || die "cannot capture the screen: $(cat "$DIR/grim")"
}
rail_drift() {
	python3 - "$1" "$2" "$3" <<'PY'
import sys
def ppm(path):
    data = open(path, "rb").read()
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        start = at
        while not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    return int(fields[1]), data[at + 1:]
(width, a), (other, b) = ppm(sys.argv[1]), ppm(sys.argv[2])
x, y, w, h = map(int, sys.argv[3].split())
changed, colours = 0, set()
for row in range(y, y + h):
    for column in range(x, x + w):
        at = (row * width + column) * 3
        colours.add(b[at:at + 3])
        changed += a[at:at + 3] != b[at:at + 3]
print(changed * 1000 // (w * h), len(colours))
PY
}
rail_box() {
	grep -o ':NAME :RAIL-TYPE :OUTPUT "[^"]*" :X [0-9]* :Y [0-9]* :WIDTH [0-9]* :HEIGHT [0-9]*' "$DIR/inspect" |
		head -1 | awk '{print $6, $8, $10, $12}'
}
surfaces() {
	grep -oE '\(:OWNER "[^"]*" :SOURCE-ID [0-9]+ :NAME :[A-Z-]+ :OUTPUT "[^"]*" :X -?[0-9]+ :Y -?[0-9]+ :WIDTH [0-9]+ :HEIGHT [0-9]+' \
		"$DIR/inspect" | grep -v 'CURSOR' | sort
}
keyed() {
	local label=$1 start
	shift
	start=$(focused)
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" "$@" >"$DIR/keys" 2>&1 || die "$label: cannot type: $(cat "$DIR/keys")"
	for _ in $(seq 60); do
		[[ $(focused) != "$start" ]] && return 0
		sleep 0.05
	done
	die "$label: the key left window $start focused"
}

[[ -n $(command -v grim) && -n $(command -v python3) ]] || die "the refusal check needs grim and python3"
mounted=$(extensions)
ctl inspect >"$DIR/inspect" || die "the session stopped answering"
held_binds=$(binds) held_theme=$(theme_of) held_focus=$(focused) held_surfaces=$(surfaces) box=$(rail_box)
[[ -n $held_binds && -n $held_theme && -n $box ]] || die "no binds, rail theme or rail surface before the refusal"
sleep 0.6
shot "$DIR/held.ppm"
cp "$DIR/init.lisp" "$DIR/good.lisp"
forms=$(grep -c '^(' "$DIR/good.lisp")
((forms >= 2)) || die "$INIT has too few top-level forms to break in the middle"
awk -v at=$((forms / 2 + 1)) '/^\(/ && ++n == at {print "(tomoe-gate-no-such-function)"} {print}' \
	"$DIR/good.lisp" >"$DIR/init.lisp"
if ctl reload >"$DIR/reload" 2>&1; then die "a config with an undefined function loaded"; fi
grep -qi 'undefined function TOMOE-GATE-NO-SUCH-FUNCTION' "$DIR/reload" ||
	die "the broken config failed without naming its error: $(cat "$DIR/reload")"
settle "$WINDOWS"
check "broken config" "$WINDOWS"
[[ $(extensions) == "$mounted" ]] || die "the refused config changed the mounted extensions"
[[ $(binds) == "$held_binds" ]] || die "the refused config changed the binds"
[[ $(theme_of) == "$held_theme" ]] || die "the refused config moved the rail off $held_theme"
[[ $(focused) == "$held_focus" ]] || die "the refused config moved focus off $held_focus"
[[ $(surfaces) == "$held_surfaces" ]] || die "the refused config changed the rail and overlay surfaces:
$(diff <(echo "$held_surfaces") <(surfaces))"
shot "$DIR/refused.ppm"
read -r drift colours < <(rail_drift "$DIR/held.ppm" "$DIR/refused.ppm" "$box")
say "broken config: $drift/1000 of the rail ($box) changed, $colours colours"
((colours >= 8)) || die "the rail ($box) is blank after the refusal"
((drift <= ${GATE_DRIFT:-300})) || die "the refused config changed $drift/1000 of the rail ($box)"
keyed "Super+Tab after the refusal" "key 125 down" "key 15 down" sync "key 15 up" "key 125 up" sync
keyed "Super+Shift+Tab after the refusal" "key 125 down" "key 42 down" "key 15 down" sync \
	"key 15 up" "key 42 up" "key 125 up" sync
await_focus "$held_focus" || die "Super+Shift+Tab after Super+Tab left $(focused) focused, not $held_focus"
seen=""
while :; do
	theme=$(theme_of)
	seen="$seen $theme"
	chord
	for _ in $(seq 60); do
		ctl inspect >"$DIR/inspect"
		[[ $(theme_of) != "$theme" ]] && break
		sleep 0.05
	done
	[[ $(theme_of) != "$theme" ]] || die "Alt+Shift+T after the refusal left the rail on $theme"
	[[ $(theme_of) == "$held_theme" ]] && break
done
[[ $(tr ' ' '\n' <<<"$seen" | sed '/^$/d' | sort | tr '\n' ' ') == "$THEMES" ]] ||
	die "Alt+Shift+T after the refusal visited$seen, the config has $THEMES"
hammer "broken config"
cp "$DIR/good.lisp" "$DIR/init.lisp"
ctl reload >"$DIR/reload" 2>&1 || die "reload after the refusal failed: $(cat "$DIR/reload")"
hammer "after reload"

if ((SOAK > 0)); then
	soak_end=$((SECONDS + SOAK)) rounds=0
	say "soaking for ${SOAK}s"
	while ((SECONDS < soak_end)); do
		rounds=$((rounds + 1))
		cycle
		ctl reload >"$DIR/reload" 2>&1 || die "soak round $rounds: reload failed: $(cat "$DIR/reload")"
		settle "$WINDOWS"
		check "soak round $rounds" "$WINDOWS"
	done
	say "soaked ${SOAK}s in $rounds rounds"
fi

say "PASS $REV $PKG"
