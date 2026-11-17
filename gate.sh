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
5120x1440@240, real test windows, dark then light, every rail theme in turn
with the output timed from the hold an idle machine is given to the frame that
carries the next key, the next pointer motion and the next client buffer, and
through the drag and the client commits that follow, its frame counter read on
both sides of it, rapid focus switches and moves with
windows closing and opening mid-animation, a reload, and a config whose middle
form calls an undefined function. It fails
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
  GATE_FRAMES_PERCENT   the share of refreshes a client that paints from its frame
                callback must present at on an active desk (default 85, median
                inside one refresh and the 99th inside two)
  GATE_WAKE_FIRST_US     how late the worst of three first frames after a hold may
                be presented (default five refreshes)
  GATE_WAKE_FIRST_MEDIAN_US  how late their median may be (default three and a half)
  GATE_WAKE_CADENCE_US   the median interval between presented frames while input
                flows (default two refreshes)
  GATE_WAKE_CADENCE_P99_US  the hundredth-percentile interval (default three)

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
	# shellcheck source=/dev/null
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
	sed -i "s#(list \"$OUTPUT\"\([^()]*\):mode (list $WIDTH $HEIGHT)#(list \"$OUTPUT\"\1:mode (list $WIDTH $HEIGHT $HZ)#" \
		"$DIR/init.lisp"
	grep -q "(list \"$OUTPUT\".*:mode (list $WIDTH $HEIGHT $HZ)" "$DIR/init.lisp" ||
		die "the config does not choose the $MODE mode of $OUTPUT by size, so the gate cannot hold that rate"
	HOLD=$(grep -o ':|SHADER-HOLD| [0-9.]*' "$DIR/init.lisp" | head -1 | awk '{print $2}')
	HOLD=${GATE_SHADER_HOLD:-${HOLD:-3}}
	PERIOD_US=$(( 1000000 / HZ ))
	SLACK_US=100
	FRAMES_PERCENT=${GATE_FRAMES_PERCENT:-70}
	PERIOD_NS=$(( 1000000000 / HZ ))
	SLACK_NS=100000
	FIRST_US=${GATE_WAKE_FIRST_US:-$(( PERIOD_US * 3 + SLACK_US ))}
	FIRST_MEDIAN_US=${GATE_WAKE_FIRST_MEDIAN_US:-$(( PERIOD_US * 2 + SLACK_US ))}
	CADENCE_US=${GATE_WAKE_CADENCE_US:-$(( PERIOD_US * 5 / 4 + SLACK_US ))}
	CADENCE_P99_US=${GATE_WAKE_CADENCE_P99_US:-$(( PERIOD_US * 3 + SLACK_US ))}
	cat >>"$DIR/init.lisp" <<'LISP'

(in-package #:tomoe-user)
(defun probe--sample (probe previous system)
  "The gate's still fixture: a reading that never moves, so a sample that draws
no new number draws nothing."
  (declare (ignore probe system))
  (or previous
      (list :percent 0 :used 0 :total 1 :failures 0)))
LISP
	cp "$INIT" "$DIR/paired.lisp"
	sed "s#$HOME/#$DIR/home/#g" "$PALETTE" >"$DIR/first.css"
	BG=$(awk '$1 == "@define-color" && $2 == "bg" {print $3}' "$DIR/first.css" | tr -d ';')
	FG=$(awk '$1 == "@define-color" && $2 == "fg" {print $3}' "$DIR/first.css" | tr -d ';')
	[[ -n $BG && -n $FG && $BG != "$FG" ]] || die "$PALETTE defines no distinct bg and fg"
	sed "s/$BG/@BG@/g; s/$FG/$BG/g; s/@BG@/$FG/g" "$DIR/first.css" >"$DIR/second.css"
	THEME_FILE=${PALETTE/#$HOME\//$DIR/home/}
	cp "$DIR/first.css" "$THEME_FILE"
	declare -p REV FINIXREV PKG INIT CLIENT KEYS SOCKET WIDTH HEIGHT HZ OUTPUT MODE WINDOWS SWITCHES SOAK HOLD \
		PERIOD_US SLACK_US FRAMES_PERCENT PERIOD_NS SLACK_NS FIRST_US FIRST_MEDIAN_US CADENCE_US CADENCE_P99_US ACCOUNT PALETTE THEME_FILE DIR \
		>"$DIR/gate.state"
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

setsid "$PKG/bin/tomoe" --socket "$SOCKET" --backend headless --no-watch --test-timings \
	--test-frame-clock --config "$DIR/init.lisp" >"$DIR/log" 2>&1 </dev/null &
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
ctl frames >"$DIR/frames-at-start" 2>&1
tr ')' '\n' <"$DIR/frames-at-start" | grep -i ":name \"$OUTPUT\"" |
	grep -qi ":refresh $((HZ * 1000))" ||
	die "$OUTPUT is not running the $MODE mode the gate times: $(grep -oi '[:]refresh [0-9]*' "$DIR/frames-at-start" | head -1)"

serial=0
open_window() {
	serial=$((serial + 1))
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" setsid "$CLIENT" --seconds 3600 --repaint \
		--color 664422 --change-to 224466 --app-id "gate-$serial" \
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

frames_on() {
	ctl frames | tr ')' '\n' | awk -v want=":NAME \"$OUTPUT\"" '
		index(toupper($0), toupper(want)) {
			for (i = 1; i <= NF; i++) if (toupper($i) == ":FRAME") print $(i + 1)
		}'
}

shot() {
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" grim -t ppm "$1" 2>"$DIR/grim" ||
		die "cannot capture the screen: $(cat "$DIR/grim")"
}

type_key() {
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" "$@" >"$DIR/keys" 2>&1 ||
		die "cannot type: $(cat "$DIR/keys")"
}

pointer() {
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" --extent "${WIDTH}x${HEIGHT}" "$@" >"$DIR/keys" 2>&1 ||
		die "cannot drive the pointer: $(cat "$DIR/keys")"
}

timings_reset() {
	ctl timings reset >"$DIR/timings" 2>&1 || die "cannot reset the timings"
}

timings_read() {
	ctl timings >"$DIR/timings" 2>&1 || die "cannot read the timings"
}

timing() {
	tr ')' '\n' <"$DIR/timings" | grep -i ":name \"$1\"" | head -1 |
		grep -oi ":$2 [0-9]*" | head -1 | awk '{print $2}'
}

focused_box() {
	ctl msg windows | awk '
		/"x":/ {gsub(/[^-0-9]/, ""); x = $0} /"y":/ {gsub(/[^-0-9]/, ""); y = $0}
		/"w":/ {gsub(/[^-0-9]/, ""); w = $0} /"h":/ {gsub(/[^-0-9]/, ""); h = $0}
		/"app_id":/ {gsub(/[^0-9]/, ""); serial = $0}
		/"focused": true/ {print x, y, w, h, serial}'
}

quiet() {
	local label=$1 before drew attempt
	sleep "$HOLD"
	for attempt in 1 2 3; do
		before=$(frames_on)
		sleep 1
		drew=$(( $(frames_on) - before ))
		((drew == 0)) && return 0
		sleep "$HOLD"
	done
	die "$label: $OUTPUT drew $drew frames in the second before the wake, so nothing was held to wake"
}

cadence() {
	local label=$1 drew=$2 p50_limit=${3:-$CADENCE_US} p99_limit=${4:-$CADENCE_P99_US}
	local share=${5:-60} p50 p99 count want
	count=$(timing present-interval count)
	local raw50 raw99
	raw50=$(timing present-interval p50)
	raw99=$(timing present-interval p99)
	[[ -n $count && -n $raw50 && -n $raw99 ]] ||
		die "$label: nothing measured the interval between frames"
	p50=$(( raw50 / 1000 )) p99=$(( raw99 / 1000 ))
	want=$(( 16 * HZ * share / 1000 ))
	((count >= want)) ||
		die "$label: presented $count frames in 1.6 s while something changed, not $share% of $HZ Hz ($want)"
	((p50 <= p50_limit)) ||
		die "$label: frames came every ${p50}us, past the ${p50_limit}us a $HZ Hz output may take"
	((p99 <= p99_limit)) ||
		die "$label: one frame in a hundred followed the last after ${p99}us, past ${p99_limit}us"
	say "$label: $count frames presented in 1.6 s ($drew drawn), interval p50 ${p50}us p99 ${p99}us"
}

wake_timed() {
	local label=$1 channel=$2 p50_limit=$3 max_limit=$4 latch_limit=$5 i value latched
	shift 5
	local -a values=() latches=() sorted=()
	for ((i = 0; i < 3; i++)); do
		quiet "$label"
		timings_reset
		"$@"
		sleep 0.4
		timings_read
		value=$(timing "$channel" max)
		latched=$(timing "latch-${channel#present-}" max)
		[[ -n $value && -n $latched ]] || die "$label: the timings did not measure that wake"
		value=$(( value / 1000 )) latched=$(( latched / 1000 ))
		values+=("$value")
		latches+=("$latched")
		printf '%s %s %s\n' "$label" "$value" "$latched" >>"$DIR/wake"
	done
	if ((latch_limit > 0)); then
		mapfile -t sorted < <(printf '%s\n' "${latches[@]}" | sort -n)
		((sorted[2] <= latch_limit)) ||
			die "$label: the frame that carried it only started ${sorted[2]}us after it, past the ${latch_limit}us an input may wait for one"
	fi
	mapfile -t sorted < <(printf '%s\n' "${values[@]}" | sort -n)
	((sorted[1] <= p50_limit)) ||
		die "$label: the first frame took ${sorted[1]}us at the median of three, past ${p50_limit}us"
	if ((max_limit > 0)); then
		((sorted[2] <= max_limit)) ||
			die "$label: the worst of three first frames took ${sorted[2]}us, past ${max_limit}us"
	fi
	say "$label: first frame ${sorted[0]}/${sorted[1]}/${sorted[2]}us (min/median/max), latched ${latches[*]}us"
}

wake_check() {
	local label=$1 box x y w h serial pid before after i
	box=$(focused_box)
	[[ -n $box ]] || die "$label: nothing is focused to wake"
	set -- $box
	x=$1 y=$2 w=$3 h=$4 serial=$5
	local cx=$(( x + w / 2 )) cy=$(( y + h / 2 ))
	wake_timed "$label key" present-input "$FIRST_MEDIAN_US" "$FIRST_US" "$(( PERIOD_US * 3 / 2 + SLACK_US ))" \
		type_key "key 42 down" sync "key 42 up" sync
	wake_timed "$label pointer" present-input "$FIRST_MEDIAN_US" "$FIRST_US" "$(( PERIOD_US * 3 / 2 + SLACK_US ))" \
		pointer "move $(( cx - 401 )) $(( cy - 199 ))"
	((serial >= 1 && serial <= ${#CLIENTS[@]})) ||
		die "$label: no client for the focused window $serial"
	pid=${CLIENTS[$((serial - 1))]}
	kill -0 "$pid" 2>/dev/null || die "$label: the focused client $pid has gone"
	wake_timed "$label damage" present-commit "$(( PERIOD_US * 7 / 2 + SLACK_US ))" 0 0 \
		kill -USR1 "$pid"

	local moves=()
	for ((i = 0; i < 500; i++)); do
		moves+=("move $(( cx - 300 + i % 160 )) $(( cy - 100 + i % 120 ))" "sleep 4")
	done
	quiet "$label motion stream"
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" --extent "${WIDTH}x${HEIGHT}" "${moves[@]}" \
		>"$DIR/keys" 2>&1 &
	pid=$!
	sleep 0.4
	timings_reset
	before=$(frames_on)
	sleep 1.6
	after=$(frames_on)
	timings_read
	kill -TERM "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	cadence "$label motion stream" $(( after - before )) "$CADENCE_US" "$CADENCE_P99_US" 90

	quiet "$label drag"
	moves=("move $cx $cy" "press 272")
	for ((i = 0; i < 400; i++)); do
		moves+=("move $(( cx + i % 160 )) $(( cy + i % 120 ))" "sleep 4")
	done
	moves+=("release 272")
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" --extent "${WIDTH}x${HEIGHT}" "${moves[@]}" \
		>"$DIR/keys" 2>&1 &
	pid=$!
	sleep 0.4
	timings_reset
	before=$(frames_on)
	sleep 1.6
	after=$(frames_on)
	timings_read
	kill -TERM "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	cadence "$label drag" $(( after - before )) "$CADENCE_US" "$CADENCE_P99_US" 60

	quiet "$label commit"
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" setsid "$CLIENT" --seconds 3600 --animate \
		--size 320x240 --color 664422 --app-id gate-wake --title gate-wake \
		>>"$DIR/clients" 2>&1 </dev/null &
	pid=$!
	sleep 0.8
	timings_reset
	before=$(frames_on)
	sleep 1.6
	after=$(frames_on)
	timings_read
	kill -TERM -- "-$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	cadence "$label commit" $(( after - before )) "$(( PERIOD_US * 5 / 2 + SLACK_US ))" "$(( PERIOD_US * 4 + SLACK_US ))" 40
	settle "$WINDOWS"

	quiet "$label resume"
	timings_reset
	type_key "key 42 down" sync "key 42 up" sync
	sleep 0.4
	timings_reset
	before=$(frames_on)
	sleep 1.6
	after=$(frames_on)
	timings_read
	cadence "$label resume" $(( after - before )) "$CADENCE_US" "$CADENCE_P99_US" 60
}

pixels_differ() {
	python3 - "$1" "$2" <<'PIXELS'
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
    return data[at + 1:]
a, b = ppm(sys.argv[1]), ppm(sys.argv[2])
print(sum(1 for i in range(0, min(len(a), len(b)), 48) if a[i:i + 3] != b[i:i + 3]))
PIXELS
}

frame_callback_check() {
	local label=$1 pid status count first last rate want p99 args="--animate" span
	[[ -n ${GATE_FRAMES_NOTHING:-} ]] && args=""
	rm -f "$DIR/frames" "$DIR/frames.err" "$DIR/since"
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" setsid "$CLIENT" --seconds 3600 $args \
		--size 320x240 --color 664422 --app-id gate-frames --title gate-frames \
		>"$DIR/frames" 2>"$DIR/frames.err" </dev/null &
	pid=$!
	sleep 2.6
	ctl msg windows >"$DIR/windows" 2>&1
	grep -q '"app_id": "gate-frames"' "$DIR/windows" ||
		die "$label: the frame-callback client never mapped a window"
	kill -TERM -- "-$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	status=$?
	((status == 0 || status == 143)) ||
		die "$label: the frame-callback client exited $status: $(head -2 "$DIR/frames.err")"
	read -r count first last < <(awk '$1 == "animate-present" {
		n++; if (!first) first = $5; last = $5; print $3 > "/dev/stderr" }
		END { print n + 0, first + 0, last + 0 }' "$DIR/frames" 2>"$DIR/since")
	((count >= 200 && last > first)) ||
		die "$label: the client reported $count presented frames of its own in 2.6 s, so it drew nothing to measure: $(head -1 "$DIR/frames.err")"
	span=$(( (last - first) / 1000000 ))
	((span > 0)) || die "$label: the client's frames span no time"
	want=$(( FRAMES_PERCENT * HZ / 100 ))
	rate=$(( (count - 1) * 1000 / span ))
	((rate >= want)) ||
		die "$label: the client presented $rate frames a second of its own (count $count over ${span}ms), not $want of $HZ Hz"
	p99=$(sort -n "$DIR/since" | awk '{ v[NR] = $1 } END { print v[int((NR - 1) * 99 / 100) + 1] }')
	((p99 <= PERIOD_NS * 4 + SLACK_NS)) ||
		die "$label: the client's commit to presentation took ${p99}ns at the 99th, past $(( PERIOD_NS * 4 + SLACK_NS ))ns"
	say "$label: the client presented $rate frames a second of its own ($count in ${span}ms), commit to presentation p99 ${p99}ns"
	settle "$WINDOWS"
}

at_rest() {
	local label=$1 attempt active_drew rest_drew moved skip
	if [[ $HOLD == 0 || $HOLD == 0.0 ]]; then
		say "$label: the config draws through, so there is no hold to read at rest or to wake"
		return 0
	fi
	moved=$(frames_on)
	sleep 1
	active_drew=$(( $(frames_on) - moved ))
	((active_drew > 0)) ||
		die "$label: the desktop drew nothing after the theme switch, so nothing animates for this check to stop"
	sleep "$HOLD"
	for attempt in 1 2 3; do
		moved=$(frames_on)
		sleep 2
		rest_drew=$(( $(frames_on) - moved ))
		((rest_drew == 0)) || continue
		say "$label: $active_drew frames in the second after the switch, 0 frames in 2 s once the machine was idle"
		if [[ -z ${SKIP_CHECKED:-} ]]; then
			shot "$DIR/held.ppm"
			type_key "key 42 down" sync "key 42 up" sync
			shot "$DIR/resumed.ppm"
			skip=$(pixels_differ "$DIR/held.ppm" "$DIR/resumed.ppm") ||
				die "$label: cannot compare the pictures around the input"
			[[ $skip =~ ^[0-9]+$ ]] || die "$label: cannot compare the pictures around the input"
			((skip <= ${GATE_HOLD_SKIP:-4000})) ||
				die "$label: the picture moved $skip sampled pixels on the input that resumed it, so it skipped ahead"
			say "$label: resumed on that input with $skip sampled pixels of change"
			SKIP_CHECKED=1
		fi
		if [[ -z ${WAKE_CHECKED:-} ]]; then
			wake_check "$label"
			WAKE_CHECKED=1
		fi
		if [[ -z ${CADENCE_CHECKED:-} ]]; then
			frame_callback_check "$label"
			CADENCE_CHECKED=1
		fi
		return 0
	done
	die "$label: $OUTPUT kept drawing $rest_drew frames in 2 s at rest"
}

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
			at_rest "$mode $theme"
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
