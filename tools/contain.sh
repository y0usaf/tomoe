#!/usr/bin/env bash
set -uo pipefail

PROG=tomoe-contain
OUTPUT=${CONTAIN_OUTPUT:-DP-4}
MODE=${CONTAIN_MODE:-5120x1440@240}
WINDOWS=${CONTAIN_WINDOWS:-3}
STALL_MS=${CONTAIN_STALL_MS:-500}
RAIL=${CONTAIN_RAIL:-bar}
CYCLE=1

say() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() {
	printf '%s: FAIL: %s\n' "$PROG" "$*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
usage: tools/contain.sh TOMOE CONFIG CLIENTS [CASE...]

Runs TOMOE (the binary) with CONFIG (an init.lisp, normally the one finix
generates) on a private headless instance inside tools/isolate (its own socket,
runtime directory, home, pid and network namespace, no X or input devices): one output at 5120x1440@240 and real
test windows that repaint every frame. CLIENTS is the directory holding
tomoe-test-client and tomoe-pointer-script. Each case then breaks something
above the core on purpose and checks that the session lost only that: it still
answers within CONTAIN_STALL_MS, its windows keep drawing and stay focused,
typed Super+Tab still moves the focus, the rail (CONTAIN_RAIL, default bar)
still has its surfaces, and the session's log file on disk
names what failed. Nothing reaches the running desktop.

Cases (default: all, in this order):
  oversize    a surface larger than one canvas
  overbudget  surfaces that together exceed the canvas budget
  hang        a reducer that never returns, on every tick
  animation   a sliding surface whose reducer errors mid-slide, every frame
  escape      reducers that keep event data past the event, by every route
  allocate    a reducer that allocates without bound
  config      a config whose middle form never returns, then one that allocates without bound
  segv        a service that segfaults, restarted every time
  mado        the config's own mado-send stopped for 8 s, then segfaulted
  stop        a Wayland client that stops reading while windows repaint
  buttons     a virtual pointer that holds 41 buttons at once
  headset     mado-solo in and out six times while a surface churns through the
              compositor's plan cache, the load that made it refuse presentations:
              each time the session must present on the right output at the right
              size, keep drawing, and take typed input; no presentation is refused
EOF
}

[[ ${1:-} == -h || ${1:-} == --help ]] && {
	usage
	exit 0
}

if [[ -n ${TOMOE_ISOLATED:-} ]]; then
	source "$TOMOE_ISOLATED/contain.state"
else
	(($# >= 3)) || {
		usage >&2
		exit 2
	}
	ISOLATE=$(dirname "$(realpath "${BASH_SOURCE[0]}")")/isolate
	"$ISOLATE" --check || exit 2
	BIN=$(readlink -f "$1") CONFIG=$(readlink -f "$2") CLIENTS=$(readlink -f "$3")
	shift 3
	CASES=("$@")
	((${#CASES[@]})) || CASES=(oversize overbudget hang animation escape allocate config segv mado stop buttons headset)
	[[ -x $BIN ]] || die "no tomoe at $BIN"
	[[ -r $CONFIG ]] || die "no config at $CONFIG"
	CLIENT=$CLIENTS/tomoe-test-client KEYS=$CLIENTS/tomoe-pointer-script
	[[ -x $CLIENT && -x $KEYS ]] || die "no test clients in $CLIENTS"
	[[ $MODE =~ ^([0-9]+)x([0-9]+)@([0-9]+)$ ]] || die "CONTAIN_MODE must be WxH@HZ"
	WIDTH=${BASH_REMATCH[1]} HEIGHT=${BASH_REMATCH[2]} HZ=${BASH_REMATCH[3]}
	DIR=$(mktemp -d "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/tomoe-contain.XXXXXX")
	SOCKET=contain-$$
	mkdir -m 700 "$DIR/run"
	mkdir -p "$DIR/home/.cache/theme" "$DIR/home/.config"
	cp -rL "$HOME/.config/fontconfig" "$DIR/home/.config/" 2>/dev/null
	sed "s#$HOME/#$DIR/home/#g" "$CONFIG" >"$DIR/init.lisp"
	[[ -r $HOME/.cache/theme/gtk.css ]] &&
		sed "s#$HOME/#$DIR/home/#g" "$HOME/.cache/theme/gtk.css" >"$DIR/home/.cache/theme/gtk.css"
	declare -p BIN CLIENT KEYS CASES WIDTH HEIGHT HZ OUTPUT SOCKET DIR >"$DIR/contain.state"
	cp "${BASH_SOURCE[0]}" "$DIR/contain.sh"
	"$ISOLATE" "$DIR" bash "$DIR/contain.sh"
	rc=$?
	if ((rc == 0)) && [[ ${CONTAIN_KEEP:-0} == 0 ]]; then rm -rf "$DIR"; else say "kept $DIR"; fi
	exit "$rc"
fi
LOG=$HOME/.local/state/tomoe/$SOCKET.log
ENV=(env)
SERVER=""
CLIENT_PIDS=()

cleanup() {
	if [[ -n $SERVER ]]; then
		timeout 3 "$BIN" --socket "$SOCKET" quit >/dev/null 2>&1
		sleep 0.5
		kill -KILL -- "-$SERVER" 2>/dev/null
	fi
	for pid in "${CLIENT_PIDS[@]}"; do kill -KILL -- "-$pid" 2>/dev/null; done
}
trap cleanup EXIT

ctl() { timeout 10 "$BIN" --socket "$SOCKET" "$@"; }

setsid "$BIN" --socket "$SOCKET" --backend headless --no-watch \
	--config "$DIR/init.lisp" >"$DIR/stderr" 2>&1 </dev/null &
SERVER=$!
for _ in $(seq 300); do ctl inspect >/dev/null 2>&1 && break; sleep 0.1; done
ctl inspect >"$DIR/inspect" 2>&1 || die "the session never answered"
grep -q ':CONFIG-ERROR NIL' "$DIR/inspect" || die "the config did not load: $(grep -o ':CONFIG-ERROR ([^)]*' "$DIR/inspect" | head -1)"

patch() {
	local name=$1 forms=$2
	ctl command patch "name=$name" "forms=(in-package #:tomoe-user)
$forms" >"$DIR/patch" 2>&1 || die "cannot patch $name: $(cat "$DIR/patch")"
}

unpatch() {
	ctl command undo-patch "name=$1" >"$DIR/patch" 2>&1 || die "cannot withdraw $1: $(cat "$DIR/patch")"
}

patch contain-screen "(define-extension \"contain-screen\" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values state (list (virtual-output \"$OUTPUT\" :mode (list $WIDTH $HEIGHT $HZ))
                      (configure-output \"HEADLESS-1\" :disabled t)) nil))"
for _ in $(seq 100); do
	ctl msg outputs 2>/dev/null | grep -q "\"w\": $WIDTH" && ! ctl msg outputs | grep -q '"HEADLESS-1"' && break
	sleep 0.1
done
ctl msg outputs | grep -q "\"w\": $WIDTH" || die "the session has no $MODE output"

serial=0
open_window() {
	serial=$((serial + 1))
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" setsid "$CLIENT" --seconds 3600 --commit-rate 10 --repaint --keys \
		--app-id "contain-$serial" --title "contain-$serial" >>"$DIR/clients" 2>&1 </dev/null &
	CLIENT_PIDS+=($!)
}

windows() { ctl msg windows 2>/dev/null | grep -c '"app_id"'; }
focused() { ctl msg windows | awk '/^    "id":/ {gsub(/[^0-9]/, ""); id = $0} /"focused": true/ {print id}'; }
frame_count() { ctl frames | grep -ioE ":name \"$1\" :frame [0-9]+" | grep -oE '[0-9]+$'; }
active_outputs() { ctl msg outputs | grep -oE '"name": "[^"]*"' | cut -d'"' -f4 | sort | tr '\n' ' '; }
title_of() { ctl msg windows | awk -v id="$1" '/^    "id":/ {gsub(/[^0-9]/, ""); now = $0} /"title":/ && now == id {gsub(/.*"title": "|",?$/, ""); print; exit}'; }
now_ms() { date +%s%3N; }

stall() {
	local count=$1 worst=0 start took
	for ((i = 0; i < count; i++)); do
		start=$(now_ms)
		ctl msg windows >"$DIR/stall" 2>&1 || {
			echo 99999
			return
		}
		took=$(($(now_ms) - start))
		((took > worst)) && worst=$took
		sleep 0.1
	done
	echo "$worst"
}

chord() {
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" "key 125 down" "key 15 down" sync "key 15 up" "key 125 up" sync \
		>"$DIR/keys" 2>&1 || die "cannot type Super+Tab: $(cat "$DIR/keys")"
}

typed_key() {
	local title before
	title=$(title_of "$(focused)")
	[[ -n $title ]] || return 1
	before=$(grep -c "^$title key 30 1$" "$DIR/clients")
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" "sleep 1200" "key 30 down" sync "key 30 up" sync "sleep 1200" >"$DIR/keys" 2>&1 ||
		die "cannot type A: $(cat "$DIR/keys")"
	for _ in $(seq 40); do
		(($(grep -c "^$title key 30 1$" "$DIR/clients") > before)) && return 0
		sleep 0.05
	done
	return 1
}

typed_focus() {
	local start now
	start=$(focused)
	[[ -n $start ]] || return 1
	chord
	for _ in $(seq 40); do
		now=$(focused)
		[[ -n $now && $now != "$start" ]] && return 0
		sleep 0.05
	done
	return 1
}

healthy() {
	local label=$1 worst before after count rail
	kill -0 "$SERVER" 2>/dev/null || die "$label: tomoe exited: $(tail -5 "$DIR/stderr")"
	worst=$(stall 10)
	((worst <= STALL_MS)) || die "$label: the session took ${worst}ms to answer: $(head -c 300 "$DIR/stall")"
	count=$(windows)
	[[ $count == "$WINDOWS" ]] || die "$label: $count windows, expected $WINDOWS"
	[[ $(ctl msg windows | grep -c '"focused": true') == 1 ]] || die "$label: no focused window"
	local output drawn=""
	for output in $(active_outputs); do
		before=$(frame_count "$output")
		sleep 0.5
		after=$(frame_count "$output")
		[[ -n $before && -n $after ]] && ((after > before)) || die "$label: $output stopped drawing ($before -> $after frames)"
		drawn+="$output $((after - before)) "
	done
	[[ -n $drawn ]] || die "$label: no output is presenting"
	if ((CYCLE)); then
		typed_focus || die "$label: typed Super+Tab did not move the focus"
	fi
	typed_key || die "$label: a typed A did not reach the focused window"
	ctl inspect >"$DIR/inspect" 2>&1 || die "$label: the session stopped answering"
	rail=$(grep -o "(:OWNER \"$RAIL\" :SOURCE-ID [0-9]* :NAME" "$DIR/inspect" | wc -l)
	((rail > 0)) || die "$label: extension $RAIL has no surfaces left"
	kill -0 "$SERVER" 2>/dev/null || die "$label: tomoe exited: $(tail -5 "$DIR/stderr")"
	say "$label: alive, slowest answer ${worst}ms, frames in 0.5s: ${drawn}typed focus $( ((CYCLE)) && echo moved || echo kept) and a typed A reached $(title_of "$(focused)"), $RAIL draws $rail surfaces"
}

logged() {
	local label=$1 pattern=$2 line
	for _ in $(seq 40); do
		line=$(grep -iE -m1 -- "$pattern" "$LOG" 2>/dev/null) && break
		sleep 0.1
	done
	[[ -n $line ]] || die "$label: $LOG has no line matching /$pattern/"
	say "$label: logged: ${line:0:240}"
}

for ((i = 0; i < WINDOWS; i++)); do open_window; done
for _ in $(seq 100); do [[ $(windows) == "$WINDOWS" ]] && break; sleep 0.05; done
sleep 0.6
healthy "baseline"

case_oversize() {
	patch contain-oversize "(define-extension \"contain-oversize\" (:reads (:windows) :state nil) (snapshot state event)
  (declare (ignore event))
  (let ((side (if (> (length (context snapshot :windows)) $WINDOWS) 16000 64)))
    (values state (list (shell-surface :huge (ui :stack) :width side :height side
                                       :anchors '(:top :left) :background #x20202080))
            nil)))"
	open_window
	WINDOWS=$((WINDOWS + 1))
	sleep 1
	healthy oversize
	logged oversize 'contain-oversize.*canvas'
	unpatch contain-oversize
	healthy "oversize withdrawn"
}

case_overbudget() {
	patch contain-overbudget "(define-extension \"contain-overbudget\" (:reads (:windows) :state nil) (snapshot state event)
  (declare (ignore event))
  (values state (loop for name in (if (> (length (context snapshot :windows)) $WINDOWS) '(:b0 :b1 :b2 :b3 :b4 :b5) '(:b0))
                      collect (shell-surface name (ui :stack) :width 5000 :height 1300 :anchors '(:top :left)
                                             :background #x10101010))
          nil))"
	open_window
	WINDOWS=$((WINDOWS + 1))
	sleep 1
	healthy overbudget
	logged overbudget 'contain-overbudget.*canvas'
	unpatch contain-overbudget
	healthy "overbudget withdrawn"
}

case_hang() {
	patch contain-hang "(define-extension \"contain-hang\" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (when (eq (getf event :type) :timer) (loop))
  (values state (list (interval :spin 50)) nil))"
	sleep 6
	healthy hang
	logged hang 'contain-hang.*(stopp|withdr|kill|disabl)'
	unpatch contain-hang 2>/dev/null || true
}

case_animation() {
	patch contain-animation "(define-extension \"contain-animation\" (:reads () :state 0) (snapshot frame event)
  (declare (ignore snapshot))
  (let ((frame (if (eq (getf event :type) :timer) (1+ frame) frame)))
    (when (> frame 30) (error \"contain: animation frame ~D failed\" frame))
    (values frame (list (interval :tick :refresh)
                        (shell-surface :slider (ui :stack) :width 240 :height 120 :anchors '(:top :left)
                                       :margin (list 200 0 0 (* 40 frame)) :slide t :background #xff3030ff))
            nil)))"
	sleep 2
	healthy animation
	logged animation 'contain-animation.*animation frame [0-9]+ failed'
	unpatch contain-animation
}

case_escape() {
	local forms="(defparameter *contain-cell* (list nil))
(defparameter *contain-list* (list :head))
(defparameter *contain-table* (make-hash-table :test #'equal))
(defparameter *contain-symbol* nil)
(defun contain--held (route state)
  (case route
    (:car (car *contain-cell*))
    (:cdr (cdr *contain-cell*))
    (:nconc (cdr *contain-list*))
    (:table (gethash \"held\" *contain-table*))
    (:symbol *contain-symbol*)
    (:intern (and state (symbol-name state)))))
(defun contain--keep (route data state)
  (case route
    (:car (setf (car *contain-cell*) data) state)
    (:cdr (setf (cdr *contain-cell*) data) state)
    (:nconc (nconc *contain-list* (list data)) state)
    (:table (setf (gethash \"held\" *contain-table*) data) state)
    (:symbol (setf *contain-symbol* data) state)
    (:intern (intern (getf data :text) :keyword))))"
	local route
	for route in car cdr nconc table symbol intern; do
		forms+="
(define-extension \"contain-escape-$route\" (:reads (:windows) :state nil) (snapshot state event)
  (let ((held (contain--held :$route state)))
    (when held (format nil \"~S ~D\" held (length (prin1-to-string held)))))
  (values (if (eq (getf event :type) :timer)
              (contain--keep :$route (list :event event :windows (context snapshot :windows)
                                           :text (format nil \"FRAME-~A-~D\" (getf event :name) (random 1000000)))
                             state)
              state)
          (list (interval :tick :refresh)) nil))"
	done
	patch contain-escape "$forms"
	sleep 3
	healthy escape
	logged escape 'contain-escape-.*escape'
	unpatch contain-escape
	healthy "escape withdrawn"
}

case_allocate() {
	patch contain-allocate "(define-extension \"contain-allocate\" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (when (eq (getf event :type) :timer)
    (let ((kept nil)) (loop (push (make-string 65536 :initial-element #\\x) kept))))
  (values state (list (interval :grow 200)) nil))"
	sleep 3
	healthy allocate
	logged allocate 'contain-allocate.*(heap|alloc|memory)'
	unpatch contain-allocate 2>/dev/null || true
}

case_config() {
	cp "$DIR/init.lisp" "$DIR/good.lisp"
	local forms
	forms=$(grep -c '^(' "$DIR/good.lisp")
	awk -v at=$((forms / 2 + 1)) '/^\(/ && ++n == at {print "(loop)"} {print}' "$DIR/good.lisp" >"$DIR/init.lisp"
	local start=$(now_ms)
	if ctl reload >"$DIR/reload" 2>&1; then die "a config that never returns loaded"; fi
	say "config: the looping config was refused after $(($(now_ms) - start))ms: $(head -c 200 "$DIR/reload")"
	healthy "config loop"
	logged "config loop" 'reload was refused.*evaluation budget'
	awk -v at=$((forms / 2 + 1)) '/^\(/ && ++n == at {print "(defparameter *contain-heap* (let ((kept nil)) (loop (push (make-string 65536) kept))))"} {print}' \
		"$DIR/good.lisp" >"$DIR/init.lisp"
	if ctl reload >"$DIR/reload" 2>&1; then die "a config that allocates without bound loaded"; fi
	say "config: the allocating config was refused: $(head -c 200 "$DIR/reload")"
	healthy "config allocate"
	logged "config allocate" 'reload was refused.*heap limit'
	cp "$DIR/good.lisp" "$DIR/init.lisp"
	ctl reload >"$DIR/reload" 2>&1 || die "the good config no longer reloads: $(cat "$DIR/reload")"
	healthy "config restored"
}

case_segv() {
	patch contain-segv "(define-extension \"contain-segv\" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values state (list (service :crasher (list \"/bin/sh\" \"-c\" \"sleep 0.3; kill -SEGV \$\$\") :restart :on-exit)) nil))"
	sleep 4
	healthy segv
	logged segv 'crasher.*signal 11'
	unpatch contain-segv
}

case_mado() {
	local sender=""
	for _ in $(seq 40); do
		sender=$(pgrep -f '/bin/mado-send ' | head -1)
		[[ -n $sender ]] && break
		sleep 0.25
	done
	[[ -n $sender ]] || die "mado: the config started no mado-send"
	kill -STOP "$sender"
	sleep 8
	healthy "mado-send stopped"
	kill -SEGV "$sender"
	kill -CONT "$sender"
	sleep 3
	healthy "mado-send segfaulted"
	logged mado 'service send of mado .*signal 11'
}

case_stop() {
	local victim
	victim=${CLIENT_PIDS[0]}
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" setsid "$CLIENT" --seconds 3600 --repaint --app-id contain-stopped \
		--title contain-stopped >>"$DIR/clients" 2>&1 </dev/null &
	victim=$!
	CLIENT_PIDS+=($victim)
	for _ in $(seq 100); do [[ $(windows) == $((WINDOWS + 1)) ]] && break; sleep 0.05; done
	kill -STOP "$victim"
	WINDOWS=$((WINDOWS + 1))
	sleep 8
	healthy "stopped client"
	kill -SEGV "$victim"
	kill -CONT "$victim"
	WINDOWS=$((WINDOWS - 1))
	for _ in $(seq 100); do [[ $(windows) == "$WINDOWS" ]] && break; sleep 0.05; done
	sleep 0.6
	healthy "stopped client killed"
}

case_buttons() {
	local presses=() releases=() button
	for button in $(seq 272 312); do
		presses+=("press $button")
		releases+=("release $button")
	done
	"${ENV[@]}" "WAYLAND_DISPLAY=$SOCKET" "$KEYS" --extent "${WIDTH}x$HEIGHT" "move 200 200" "${presses[@]}" sync \
		"${releases[@]}" sync >"$DIR/keys" 2>&1 || die "buttons: the pointer client failed: $(cat "$DIR/keys")"
	healthy buttons
	logged buttons 'already holds [0-9]+ buttons'
}

case_headset() {
	patch contain-churn "(define-extension \"contain-churn\" (:reads () :state 0) (snapshot tick event)
  (declare (ignore snapshot))
  (let ((tick (if (eq (getf event :type) :timer) (1+ tick) tick)))
    (values tick (list (interval :churn 4)
                       (shell-surface :ticker (ui :text :text (format nil \"~D\" tick))
                                      :anchors '(:top :left) :layer :overlay)
                       (shell-surface (intern (format nil \"CHURN-~D\" tick) :keyword)
                                      (ui :text :text (format nil \"~D\" tick))
                                      :anchors '(:bottom :left) :layer :overlay))
            nil)))"
	local lap want size got
	for lap in 1 2 3 4 5 6; do
		ctl command mado-solo >"$DIR/command" 2>&1 || die "headset $lap: mado-solo failed: $(cat "$DIR/command")"
		if ((lap % 2)); then want="MADO-1 " size='"w": 3840,"h": 2160'; else want="$OUTPUT " size="\"w\": $WIDTH,\"h\": $HEIGHT"; fi
		for _ in $(seq 80); do
			got=$(active_outputs)
			[[ $got == "$want" ]] && break
			sleep 0.05
		done
		[[ $got == "$want" ]] || die "headset $lap: the session presents on '$got', expected '$want'"
		ctl msg outputs | tr -d ' \n' | grep -q "${size// /}" || die "headset $lap: $want is not at $size: $(ctl msg outputs | tr -d ' \n')"
		if ((lap % 2)); then CYCLE=0; else CYCLE=1; fi
		healthy "headset $lap, on ${want% }"
	done
	if grep -iE 'retained shell rendering|no resolved plan|plans changed' "$LOG" "$DIR/stderr"; then
		die "headset: a presentation was refused while the surfaces churned"
	fi
	CYCLE=1
	say "headset: 6 transitions under surface churn, no presentation refused"
	unpatch contain-churn
}

for c in "${CASES[@]}"; do
	declare -F "case_$c" >/dev/null || die "no case $c"
	say "case $c"
	"case_$c"
done
say "PASS ${CASES[*]}"
