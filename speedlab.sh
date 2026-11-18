#!/usr/bin/env bash
set -uo pipefail

PROG=tomoe-speedlab
SYSTEM=${SPEEDLAB_SYSTEM:-x86_64-linux}
CONFIG=${SPEEDLAB_CONFIG:-$HOME/.config/tomoe/init.lisp}
PALETTE=${SPEEDLAB_PALETTE:-$HOME/.cache/theme/gtk.css}
OUTPUT=${SPEEDLAB_OUTPUT:-DP-4}
MODE=${SPEEDLAB_MODE:-5120x1440@240}
CACHE=${SPEEDLAB_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/tomoe-speedlab}
BASE_RUNS=${SPEEDLAB_BASE_RUNS:-3}
LOCK=${TOMOE_LOCK:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/agent/tomoe-vm.lock}
HERE=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
REFERENCE=${SPEEDLAB_REFERENCE:-}
OUT=""
FRESH=0
ONLY=""

usage() {
	cat <<'EOF'
usage: speedlab.sh [--reference BUILD] [--out FILE] [--fresh] [--persona A|B] CANDIDATE
       speedlab.sh --compare BASE CANDIDATE
       speedlab.sh --self-test
       speedlab.sh --profile BUILD

Runs CANDIDATE (a store path with bin/tomoe, a build directory with build/tomoe or
tomoe, or a git revision of this repository, built with nix) through the recorded
workload in bench/speedlab/workload on a private headless instance, twice, and
prints one JSON verdict: the cost (user-space instructions of the compositor and
everything it starts), peak and settled memory with a breakdown, and whether
every output frame and every protocol event the rig's clients saw match the
reference build's run.

  --reference BUILD   the build to compare against (default: bench/speedlab/reference)
  --compare BASE CANDIDATE
                      judge CANDIDATE against BASE instead of the frozen reference, with the
                      same equivalence rules; "outcome" says win, loss, mixed or neutral from
                      the per-metric deltas that leave the noise band
  --out FILE          also write the verdict, with both runs, to FILE
  --fresh             recompute the reference instead of using its cached runs
  --persona A|B       run only that persona and print its raw result

The reference is run SPEEDLAB_BASE_RUNS (default 3, never fewer than 3) times and
the verdict scores against their mean, so one base measurement's noise cannot
decide a climb.

SPEEDLAB_CONFIG (default ~/.config/tomoe/init.lisp) and SPEEDLAB_PALETTE (default
~/.cache/theme/gtk.css) are snapshotted into the run; nothing reaches the running
desktop.
EOF
}

say() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() {
	printf '%s: FAIL: %s\n' "$PROG" "$*" >&2
	exit 2
}

. "$HERE/tools/lease.sh"

rig() {
	lease_take "$LOCK" -s 0 || die "cannot take $LOCK"
	RIG=$(nix build --no-link --print-out-paths "$HERE#legacyPackages.$SYSTEM.speedlab" 2>"$WORK/rig.log") ||
		{ lease_release; cat "$WORK/rig.log" >&2; die "cannot build the rig"; }
	lease_release
	# shellcheck source=/dev/null
	. "$RIG/bin/speedlab-env"
}

resolve() {
	local what=$1 binary=""
	if [[ -x $what/bin/tomoe ]]; then
		binary=$what/bin/tomoe
	elif [[ -x $what/build/tomoe && -f $what/build/tomoe ]]; then
		binary=$what/build/tomoe
	elif [[ -f $what/tomoe && -x $what/tomoe ]]; then
		binary=$what/tomoe
	elif [[ -f $what && -x $what ]]; then
		binary=$what
	elif rev=$(git -C "$HERE" rev-parse --verify --quiet "$what^{commit}"); then
		lease_take "$LOCK" -s 0 || die "cannot take $LOCK"
		local out
		out=$(nix build --no-link --print-out-paths "git+file://$HERE?rev=$rev#packages.$SYSTEM.default" \
			2>"$WORK/build.log") || { lease_release; cat "$WORK/build.log" >&2; die "cannot build $what"; }
		lease_release
		binary=$out/bin/tomoe
	else
		die "$what is neither a build nor a revision"
	fi
	realpath "$binary"
}

stage() {
	local stage=$WORK/stage width height hz init
	[[ $MODE =~ ^([0-9]+)x([0-9]+)@([0-9]+)$ ]] || die "SPEEDLAB_MODE must be WxH@HZ"
	width=${BASH_REMATCH[1]} height=${BASH_REMATCH[2]} hz=${BASH_REMATCH[3]}
	[[ -r $CONFIG ]] || die "no config at $CONFIG"
	[[ -r $PALETTE ]] || die "no palette at $PALETTE"
	mkdir -p "$stage/home/.config/tomoe" "$stage/home/.cache/theme" "$stage/home/.local/share/icons"
	id -un >"$stage/account"
	id -u >"$stage/uid"
	id -g >"$stage/gid"
	printf '%s\n' "$HOME" >"$stage/home-path"
	printf '%s\n' "$OUTPUT" >"$stage/output"
	printf '%s %s\n' "$width" "$height" >"$stage/extent"
	printf '%s\n' "${XCURSOR_THEME:-default}" >"$stage/cursor-theme"
	printf '%s\n' "${XCURSOR_SIZE:-24}" >"$stage/cursor-size"
	local node=${SPEEDLAB_RENDER_NODE:-} device
	if [[ -z $node ]]; then
		for device in /dev/dri/renderD*; do
			[[ -r $device && -w $device ]] || continue
			[[ $(readlink "/sys/class/drm/${device##*/}/device/driver") == */nvidia ]] && continue
			node=$device
			break
		done
	fi
	[[ -n $node ]] || die "no render node mesa can allocate on; set SPEEDLAB_RENDER_NODE"
	printf '%s\n' "$node" >"$stage/render-node"
	cp "$SPEEDLAB_SHARE/workload" "$stage/workload"
	cp "$PALETTE" "$stage/palette-dark"
	python3 - "$stage/palette-dark" "$stage/palette-light" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
colors = dict(re.findall(r"@define-color\s+(\S+)\s+(#[0-9A-Fa-f]{6})", text))
bg, fg = colors["bg"], colors["fg"]
def luminance(c):
    return sum(w * int(c[i:i + 2], 16) for w, i in ((0.2126, 1), (0.7152, 3), (0.0722, 5)))
def swap(text, a, b):
    return text.replace(a, "@SWAP@").replace(b, a).replace("@SWAP@", b)
dark_bg, dark_fg = (bg, fg) if luminance(bg) < luminance(fg) else (fg, bg)
dark = text if dark_bg == bg else swap(text, bg, fg)
light = swap(dark, dark_bg, dark_fg)
open(sys.argv[1], "w").write(dark)
open(sys.argv[2], "w").write(light)
PY
	init=$stage/home/.config/tomoe/init.lisp
	sed "s#(list \"$OUTPUT\"\([^()]*\):mode (list $width $height)#(list \"$OUTPUT\"\1:mode (list $width $height $hz)#" \
		"$(realpath "$CONFIG")" >"$init"
	cat >>"$init" <<LISP

(in-package #:tomoe-user)
(defun probe--sample (probe previous system)
  "The rig's still fixture: a reading that never moves."
  (declare (ignore probe system))
  (or previous
      (list :percent 0 :used 0 :total 1 :failures 0)))
(define-extension "speedlab-screen" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values state (list (virtual-output "$OUTPUT" :mode (list $width $height $hz))
                      (configure-output "HEADLESS-1" :disabled t)) nil))
LISP
	[[ -d $HOME/.config/fontconfig ]] && cp -rL --no-preserve=mode "$HOME/.config/fontconfig" "$stage/home/.config/"
	[[ -d $HOME/.cache/fontconfig ]] && mkdir -p "$stage/home/.cache" && cp -rL --no-preserve=mode "$HOME/.cache/fontconfig" "$stage/home/.cache/"
	local theme
	for theme in "${XCURSOR_THEME:-}" hicolor; do
		[[ -n $theme && -d $HOME/.local/share/icons/$theme ]] &&
			cp -rL --no-preserve=mode "$HOME/.local/share/icons/$theme" "$stage/home/.local/share/icons/"
	done
	[[ -n ${TRACE:-} ]] && : >"$stage/trace"
	[[ -n ${DUMP:-} ]] && printf '%s\n' "${DUMP//,/ }" >"$stage/dump-frames"
	(cd "$stage" && find . -type f -print0 | sort -z | xargs -0 sha256sum) | sha256sum | cut -c1-16 >"$WORK/stage.hash"
}

run() {
	local binary=$1 persona=$2 result=$3 dir offset
	dir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/agent/speedlab-run.XXXXXX") || die "no run directory"
	cp -a "$WORK/stage" "$dir/stage"
	local target=$binary
	if [[ $binary != /nix/store/* ]]; then
		mkdir -p "$dir/stage/bin"
		cp "$binary" "$dir/stage/bin/tomoe"
		target=/run/speedlab/stage/bin/tomoe
	fi
	offset=0
	[[ $persona == B ]] && offset=$(( (RANDOM << 15 | RANDOM) % 2000000 + 86400 ))
	env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u TOMOE_SOCKET \
		"$HERE/tools/isolate" "$dir" env SPEEDLAB_TOOLS="$SPEEDLAB_TOOLS" SPEEDLAB_SHARE="$SPEEDLAB_SHARE" \
		SPEEDLAB_MESA="$SPEEDLAB_MESA" SPEEDLAB_PATH="$SPEEDLAB_PATH" SPEEDLAB_PYTHON="$SPEEDLAB_PYTHON" \
		SPEEDLAB_BASH="$SPEEDLAB_BASH" SPEEDLAB_PERF="$SPEEDLAB_PERF" \
		"$SPEEDLAB_BASH" "$SPEEDLAB_SHARE/seal.sh" "$dir" "$persona" "$offset" "$target" ${PROFILE_TOOL:+"$PROFILE_TOOL"} \
		>"$result" 2>"$result.err"
	local rc=$?
	chmod -R u+w "$dir" 2>/dev/null; rm -rf "$dir"
	[[ -s $result ]] || die "the $persona run produced nothing (exit $rc): $(tail -20 "$result.err")"
}

main_run() {
	local binary=$1 reference key cached index pid temp
	local -a pids=()
	[[ -n $REFERENCE ]] || REFERENCE=$(<"$HERE/bench/speedlab/reference")
	reference=$(resolve "$REFERENCE")
	key=$(printf '%s %s %s %s %s' "$RIG" "$reference" "$(sha256sum "$reference" | cut -c1-64)" \
		"$(<"$WORK/stage.hash")" "$BASE_RUNS" | sha256sum | cut -c1-32)
	cached=$CACHE/$key.json
	mkdir -p "$CACHE"
	if ((FRESH)) || [[ ! -s $cached ]]; then
		say "running the reference $reference $BASE_RUNS times"
		for ((index = 1; index <= BASE_RUNS; index++)); do
			run "$reference" A "$WORK/reference-$index.json" &
			pids+=($!)
		done
		for pid in "${pids[@]}"; do wait "$pid" || exit 2; done
		temp=$(mktemp "$CACHE/$key.XXXXXX") || die "cannot write the cache"
		if ! "$SPEEDLAB_PYTHON" - "$WORK" "$BASE_RUNS" "$temp" <<'PY'
import json, sys
work, runs, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
bases = [json.load(open(f"{work}/reference-{index}.json")) for index in range(1, runs + 1)]
failed = [f"run {index}: {base.get('error', '?')}" for index, base in enumerate(bases, 1) if not base.get("ok")]
if failed:
    sys.exit("; ".join(failed))
with open(out, "w") as cache:
    json.dump(bases, cache)
PY
		then
			rm -f "$temp"
			die "a reference run failed, so there is no baseline to cache"
		fi
		mv "$temp" "$cached" || { rm -f "$temp"; die "cannot publish the baseline at $cached"; }
	fi
	say "running $binary as personas A and B"
	run "$binary" A "$WORK/a.json" &
	local first=$!
	run "$binary" B "$WORK/b.json" || exit 2
	wait "$first" || exit 2
	"$SPEEDLAB_PYTHON" "$SPEEDLAB_SHARE/verdict.py" "$cached" "$WORK/a.json" "$WORK/b.json" \
		"$reference" "$binary" ${OUT:+"$OUT"}
}

profile() {
	local binary=$1 tool
	local -a pids=()
	say "profiling $binary: perf samples every 10M user instructions, a heap census at every phase,"
	say "counted runs with and without the rig's frame capture to size its overhead, then a main-thread call graph"
	for tool in perf census plain bare; do
		if [[ $tool == plain ]]; then
			run "$binary" A "$WORK/$tool.json" &
		else
			PROFILE_TOOL=$tool run "$binary" A "$WORK/$tool.json" &
		fi
		pids+=($!)
	done
	for pid in "${pids[@]}"; do wait "$pid" || exit 2; done
	PROFILE_TOOL=callgraph run "$binary" A "$WORK/callgraph.json"
	PATH=$SPEEDLAB_PATH:$PATH "$SPEEDLAB_PYTHON" "$SPEEDLAB_SHARE/report.py" "$WORK/perf.json" "$WORK/callgraph.json" \
		"$WORK/census.json" "$WORK/plain.json" "$WORK/bare.json" "$binary" ${OUT:+"$OUT"}
}

build_tree() {
	local tree=$1 out
	lease_take "$LOCK" -s 0 || die "cannot take $LOCK"
	out=$(nix build --no-link --print-out-paths "path:$tree#packages.$SYSTEM.default" 2>"$tree.log") ||
		{ lease_release; tail -30 "$tree.log" >&2; die "cannot build $tree"; }
	lease_release
	printf '%s\n' "$out"
}

self_test() {
	local base=$WORK/self name expect verdict reasons failures=0 clean
	local -a names=(clean)
	for patch in "$HERE"/bench/speedlab/cheats/*.patch; do names+=("$(basename "$patch" .patch)"); done
	for name in "${names[@]}"; do
		mkdir -p "$base/$name"
		git -C "$HERE" archive HEAD | tar -x -C "$base/$name"
		if [[ $name != clean ]]; then
			git -C "$base/$name" apply "$HERE/bench/speedlab/cheats/$name.patch" || die "cannot apply the $name cheat"
		fi
	done
	say "building the clean tree and $((${#names[@]} - 1)) seeded candidates"
	local -a pids=()
	for name in "${names[@]}"; do
		build_tree "$base/$name" >"$base/$name.out" &
		pids+=($!)
	done
	for pid in "${pids[@]}"; do wait "$pid" || die "a self-test build failed"; done
	clean=$(<"$base/clean.out")
	REFERENCE=$clean
	printf '%-7s %-7s %s\n' build verdict reasons
	for name in "${names[@]}"; do
		verdict=$(main_run "$(<"$base/$name.out")/bin/tomoe" 2>"$base/$name.err")
		printf '%s\n' "$verdict" >"$base/$name.json"
		reasons=$(printf '%s' "$verdict" | "$SPEEDLAB_PYTHON" -c 'import json, sys
v = json.load(sys.stdin)
print(v["verdict"], "|", " || ".join(v["reasons"]) or "-")')
		printf '%-7s %s\n' "$name" "$reasons"
		expect=$(sed -n "s/^$name //p" "$HERE/bench/speedlab/cheats/expect")
		if [[ $name == clean ]]; then
			[[ $reasons == accept* ]] || { failures=$((failures + 1)); say "the clean build was not accepted"; }
		elif [[ $reasons != reject* || $reasons != *"$expect"* ]]; then
			failures=$((failures + 1))
			say "$name was not rejected for \"$expect\""
		fi
	done
	((failures == 0)) || die "$failures of ${#names[@]} self-test builds misjudged"
	say "self-test passed: the clean build is accepted and every seeded cheat is rejected for its own reason"
}

main() {
	[[ ${1:-} == -h || ${1:-} == --help ]] && {
		usage
		exit 0
	}
	local persona="" candidate="" mode=verdict
	while (($#)); do
		case $1 in
		--self-test) mode=self-test ;;
		--profile) mode=profile ;;
		--reference) REFERENCE=$2; shift ;;
		--compare) REFERENCE=$2; candidate=$3; shift 2 ;;
		--out) OUT=$2; shift ;;
		--fresh) FRESH=1 ;;
		--persona) persona=$2; shift ;;
		--dump-frames) DUMP=$2; shift ;;
		--trace) TRACE=1 ;;
		-*) usage >&2; exit 2 ;;
		*) candidate=$1 ;;
		esac
		shift
	done
	[[ -n $candidate || $mode == self-test ]] || { usage >&2; exit 2; }
	[[ $BASE_RUNS =~ ^[0-9]+$ ]] && ((BASE_RUNS >= 3)) || die "SPEEDLAB_BASE_RUNS must be at least 3, not '$BASE_RUNS'"
	env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u TOMOE_SOCKET "$HERE/tools/isolate" --check || exit 2
	WORK=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/agent/speedlab.XXXXXX")
	trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
	rig
	stage
	if [[ $mode == self-test ]]; then
		self_test
		return
	fi
	local binary
	binary=$(resolve "$candidate")
	if [[ $mode == profile ]]; then
		profile "$binary"
		return
	fi
	if [[ -n $persona ]]; then
		run "$binary" "$persona" "$WORK/run.json"
		cat "$WORK/run.json"
		return
	fi
	main_run "$binary"
}

main "$@"; exit
