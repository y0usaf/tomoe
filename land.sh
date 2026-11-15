#!/usr/bin/env bash
set -uo pipefail

PROG=tomoe-land
ROOT=${TOMOE_LAND_ROOT:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/agent/tomoe-land}
LOCK=${TOMOE_LOCK:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/agent/tomoe-vm.lock}
PENDING=${TOMOE_PENDING:-$(dirname "$LOCK")/tomoe-vm.pending}
AGENT=${TOMOE_AGENT:-tomoe-land}
SYSTEM=${LAND_SYSTEM:-x86_64-linux}
BASE=${LAND_BASE:-origin/main}
FINIX=${LAND_FINIX:-$HOME/finix}
FINIX_REV=""
SMOKE=${LAND_SMOKE:-smoke}
NIXCORES=${LAND_NIX_CORES:-4}
LEASEWAIT=${LAND_LEASE_WAIT:-10800}
PENDINGWAIT=${LAND_PENDING_WAIT:-600}
LANDPEND=${LAND_PENDING_WAIT_LAND:-30}
SMOKE_TIMEOUT=${LAND_SMOKE_TIMEOUT:-1200}
TIMING_TIMEOUT=${LAND_TIMING_TIMEOUT:-1800}
KILLWAIT=${LAND_KILL_AFTER:-60}
KEEP=${LAND_KEEP:-0}

QUEUE=$ROOT/queue
ACTIVE=$ROOT/active
RESULTS=$ROOT/results
LOGS=$ROOT/logs
DONE=$ROOT/done

say() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() {
	printf '%s: %s\n' "$PROG" "$*" >&2
	exit 1
}

for d in "$QUEUE" "$ACTIVE" "$RESULTS" "$LOGS" "$DONE"; do mkdir -p "$d"; done
REPO=$(git rev-parse --git-common-dir 2>/dev/null) || REPO=""
[[ -n $REPO ]] && REPO=$(cd "$REPO" && pwd)

usage() {
	cat <<'EOF'
usage: tomoe-land <command> [args]

  daemon                          run the queue (one job at a time, in order)
  submit <sha|branch>             land that commit: rebase, smoke, push both remotes
  submit --timing [-s SUITE] [--timeout SEC] [REF]
                                  measure one suite in its own VM, time boxed
  wait <id> [-t SEC]              block until the result file exists, then print it
  status                          queue, active job and the last results
  cancel <id>                     remove a queued job, or kill the running one

Paths: queue, active and results live under $TOMOE_LAND_ROOT
(default ${XDG_RUNTIME_DIR}/agent/tomoe-land). The lock is
${XDG_RUNTIME_DIR}/agent/tomoe-vm.lock; timing jobs and smoke runs take it
exclusively, builds take it shared.
EOF
}

repo_root() {
	local dir
	dir=$(git rev-parse --git-common-dir 2>/dev/null) || return 1
	(cd "$dir" && pwd)
}

note_step() {
	printf '%s %s %s\n' "$1" "$2" "$3" >>"$ROOT/steps"
}

take_lease() {
	local mode=$1 pendmax=${2:-$PENDINGWAIT} waited=0
	if [[ $mode == -x ]]; then
		mkdir -p "$PENDING" || return 1
		: >"$PENDING/$AGENT.$$" || return 1
	elif [[ -d $PENDING ]]; then
		while [[ -n $(ls -A "$PENDING" 2>/dev/null) ]] && ((waited < pendmax)); do
			sleep 2
			waited=$((waited + 2))
		done
	fi
	exec 9>"$LOCK" || return 1
	if ! flock "$mode" -w "$LEASEWAIT" 9; then
		rm -f "$PENDING/$AGENT.$$"
		return 75
	fi
	rm -f "$PENDING/$AGENT.$$"
	return 0
}

release_lease() { exec 9>&-; }

submit() {
	local type=land ref="" suite=timing timeout=""
	while (($#)); do
		case $1 in
		--timing | --bench) type=timing ;;
		-s | --suite)
			shift
			suite=${1:-}
			;;
		--timeout)
			shift
			timeout=${1:-}
			;;
		-*) die "unknown option $1" ;;
		*) ref=$1 ;;
		esac
		shift
	done
	if [[ $type == timing && -z $ref ]]; then ref=$BASE; fi
	[[ -n $ref ]] || die "submit needs a commit or branch"
	local ns id file tmp
	ns=$(date +%s%N)
	id=$ns
	file=$QUEUE/$ns-$id
	tmp=$ROOT/.submitting/$$-$ns
	mkdir -p "$ROOT/.submitting" || die "cannot write the submission"
	printf '%s\t%s\t%s\t%s\n' "$type" "$ref" "$suite" "$timeout" >"$tmp" ||
		die "cannot write the submission"
	mv "$tmp" "$file" || die "cannot queue the submission"
	printf '%s queued %s %s %s\n' "$(date -u +%FT%TZ)" "$id" "$type" "$ref" >>"$ROOT/journal.log"
	say "queued $id in $file"
	rm -f "$RESULTS/$id"
	printf '%s\n' "$id"
}

wait_result() {
	local id="" limit=""
	while (($#)); do
		case $1 in
		-t | --timeout)
			shift
			limit=${1:-}
			;;
		*) id=$1 ;;
		esac
		shift
	done
	[[ -n $id ]] || die "wait needs an id"
	local waited=0
	while [[ ! -f $RESULTS/$id ]]; do
		[[ -n $limit ]] && ((waited >= limit)) && die "no result for $id after ${limit}s"
		sleep 0.5
		waited=$((waited + 1))
	done
	[[ -f $RESULTS/$id ]] || die "no result for $id"
	while ! grep -q '"finished"' "$RESULTS/$id" 2>/dev/null; do sleep 0.2; done
	cat "$RESULTS/$id"
}

cancel() {
	local id=${1:-}
	[[ -n $id ]] || die "cancel needs an id"
	local matcher
	for matcher in "$QUEUE"/*-"$id" "$ACTIVE"/*-"$id"; do
		[[ -e $matcher ]] || continue
		if [[ $matcher == "$ACTIVE"/* ]]; then
			local pid
			pid=$(cat "$matcher.pid" 2>/dev/null)
			[[ -n $pid ]] && kill -TERM -- -"$pid" 2>/dev/null
			printf '{"id":"%s","status":"cancelled","finished":"%s"}\n' "$id" \
				"$(date -u +%FT%TZ)" >"$RESULTS/.$id.tmp"
			mv "$RESULTS/.$id.tmp" "$RESULTS/$id"
			say "cancelled $id"
			return 0
		fi
		mv "$matcher" "$DONE/" 2>/dev/null || rm -f "$matcher"
		printf '{"id":"%s","status":"cancelled","finished":"%s"}\n' "$id" \
			"$(date -u +%FT%TZ)" >"$RESULTS/.$id.tmp"
		mv "$RESULTS/.$id.tmp" "$RESULTS/$id"
		say "dropped $id from the queue"
		return 0
	done
	die "no queued or active job $id"
}

status() {
	printf 'queue (first is next):\n'
	local f id
	for f in $(ls "$QUEUE" 2>/dev/null | sort); do
		id=${f#*-}
		printf '  %s  %s\n' "$id" "$(tr '\t' ' ' <"$QUEUE/$f")"
	done
	printf 'active:\n'
	for f in $(ls "$ACTIVE" 2>/dev/null | sort); do
		id=${f#*-}
		printf '  %s  %s\n' "$id" "$(tr '\t' ' ' <"$ACTIVE/$f")"
	done
	printf 'recent results:\n'
	for f in $(ls -t "$RESULTS" 2>/dev/null | head -8); do
		printf '  %s  %s\n' "$f" "$(tr -d '\n' <"$RESULTS/$f" | cut -c1-160)"
	done
}

run_land() {
	local id=$1 ref=$2
	local log=$LOGS/$id
	mkdir -p "$log"
	local repo tree sha base_sha tip rc=0
	repo=$(repo_root) || {
		finish "$id" failed "" "" "no git repository here" ""
		return 1
	}
	git -C "$repo" fetch --quiet origin || {
		finish "$id" failed "$ref" "" resolve "$log/resolve.log"
		return 1
	}
	git -C "$repo" fetch --quiet forgejo >/dev/null 2>&1 || true
	base_sha=$(git -C "$repo" rev-parse "$BASE^{commit}") || {
		finish "$id" failed "$ref" "" resolve "$log/resolve.log"
		return 1
	}
	sha=$(git -C "$repo" rev-parse --verify --quiet "refs/remotes/origin/$ref^{commit}")
	[[ -n $sha ]] || sha=$(git -C "$repo" rev-parse --verify --quiet "$ref^{commit}")
	if [[ -z $sha ]]; then
		printf '%s is not a commit here\n' "$ref" >"$log/resolve.log"
		finish "$id" failed "$ref" "$base_sha" resolve "$log/resolve.log"
		return 1
	fi
	printf 'ref %s\nsha %s\nbase %s\n' "$ref" "$sha" "$base_sha" >"$log/resolve.log"
	: >"$ROOT/steps"
	note_step resolve passed "$log/resolve.log"

	tree=$ROOT/work/$id
	rm -rf "$tree"
	git -C "$repo" worktree prune >/dev/null 2>&1 || true
	git -C "$repo" worktree add --quiet --detach "$tree" "$sha" >"$log/rebase.log" 2>&1 || {
		printf 'cannot create a worktree for %s\n' "$sha" >>"$log/rebase.log"
		finish "$id" failed "$sha" "$base_sha" rebase "$log/rebase.log"
		return 1
	}
	if ! git -C "$tree" merge-base --is-ancestor "$base_sha" HEAD; then
		git -C "$tree" -c user.name=tomoe-land -c user.email=tomoe-land@localhost \
			rebase "$base_sha" >>"$log/rebase.log" 2>&1 || {
			git -C "$tree" rebase --abort >/dev/null 2>&1
			printf 'the rebase conflicted\n' >>"$log/rebase.log"
			cleanup_tree "$repo" "$tree"
			finish "$id" failed "$sha" "$base_sha" rebase "$log/rebase.log"
			return 1
		}
	fi
	tip=$(git -C "$tree" rev-parse HEAD)
	printf 'tip %s\n' "$tip" >>"$log/rebase.log"
	note_step rebase passed "$log/rebase.log"

	if ! run_smoke "$id" "$tree" "$log/smoke.log"; then
		cleanup_tree "$repo" "$tree"
		finish "$id" failed "$tip" "$base_sha" smoke "$log/smoke.log"
		return 1
	fi
	note_step smoke passed "$log/smoke.log"
	[[ -f $RESULTS/$id ]] && {
		cleanup_tree "$repo" "$tree"
		return 1
	}

	push_landing "$id" "$repo" "$tip" "$base_sha" "$ref" "$log/push.log"
	case $? in
	0)
		note_step push passed "$log/push.log"
		note_step forgejo passed "$log/push.log.forgejo"
		printf '%s\n' "$tip" >"$ROOT/undeployed"
		;;
	2)
		note_step push passed "$log/push.log"
		note_step forgejo failed "$log/push.log.forgejo"
		printf '%s\n' "$tip" >"$ROOT/undeployed"
		;;
	*)
		cleanup_tree "$repo" "$tree"
		finish "$id" failed "$tip" "$base_sha" push "$log/push.log"
		return 1
		;;
	esac
	if pin_finix "$log/pin.log" "$tip"; then
		note_step pin passed "$log/pin.log"
	else
		note_step pin failed "$log/pin.log"
	fi
	cleanup_tree "$repo" "$tree"
	finish "$id" passed "$tip" "$tip" "" ""
	return 0
}

cleanup_tree() {
	if ((KEEP)); then return 0; fi
	git -C "$1" worktree remove --force "$2" >/dev/null 2>&1 || true
	git -C "$1" worktree prune >/dev/null 2>&1 || true
}

run_smoke() {
	local id=$1 tree=$2 log=$3 driver rc=0
	{
		printf 'land: building the smoke driver\n'
	} >>"$log"
	if ! take_lease -s "$LANDPEND"; then
		printf 'land: the shared lease timed out\n' >>"$log"
		return 75
	fi
	driver=$(
		cd "$tree" || exit 1
		nice -n 10 nix build --no-link --print-out-paths --max-jobs 1 \
			--option cores "$NIXCORES" ".#checks.$SYSTEM.$SMOKE.driver"
	) >>"$log" 2>&1
	release_lease
	[[ -f $RESULTS/$id ]] && {
		printf 'land: cancelled before the run\n' >>"$log"
		return 75
	}
	if [[ -z $driver ]]; then
		printf 'land: the smoke driver did not build\n' >>"$log"
		return 76
	fi
	if ! take_lease -x; then
		printf 'land: the exclusive lease timed out\n' >>"$log"
		return 75
	fi
	mkdir -p "$ROOT/run/$id/out"
	local pid
	set -m
	timeout --signal=TERM --kill-after="$KILLWAIT" "$SMOKE_TIMEOUT" \
		bash -c 'exec 9>&-; cd "$1" && LOGFILE=/dev/null nice -n 10 "$2/bin/nixos-test-driver" -o "$1/out"' _ \
		"$ROOT/run/$id" "$driver" >>"$log" 2>&1 &
	pid=$!
	set +m
	printf '%s\n' "$pid" >"$ACTIVE_FILE.pid"
	wait "$pid" || rc=$?
	rm -f "$ACTIVE_FILE.pid"
	release_lease
	case $rc in
	124 | 137)
		printf 'land: smoke hit its %ss limit\n' "$SMOKE_TIMEOUT" >>"$log"
		return 1
		;;
	esac
	if grep -q 'tomoe suite passed:' "$log"; then
		printf 'land: smoke passed\n' >>"$log"
		return 0
	fi
	printf 'land: smoke did not report a passing suite (exit %s)\n' "$rc" >>"$log"
	return 1
}

pin_finix() {
	local log=$1 tip=$2 rev short remote=github
	[[ -d $FINIX/.git ]] || {
		printf 'no finix checkout at %s\n' "$FINIX" >"$log"
		return 1
	}
	if ! git -C "$FINIX" diff --quiet --cached; then
		printf '%s has staged changes; not touching its index\n' "$FINIX" >"$log"
		return 1
	fi
	if ! git -C "$FINIX" diff --quiet -- flake.lock; then
		printf '%s has an uncommitted flake.lock; not overwriting it\n' "$FINIX" >"$log"
		return 1
	fi
	short=$(git -C "$REPO" rev-parse --short "$tip") || return 1
	git -C "$FINIX" remote 2>/dev/null | grep -qx "$remote" || remote=origin
	(
		cd "$FINIX" || exit 1
		git fetch --quiet "$remote" || exit 1
		nix flake update tomoe || exit 1
		git add flake.lock || exit 1
		git -c user.name=tomoe-land -c user.email=tomoe-land@localhost commit --quiet \
			-m "finix: pin tomoe $short" -- flake.lock || exit 1
		git push "$remote" HEAD:main || exit 1
	) >"$log" 2>&1 || return 1
	rev=$(git -C "$FINIX" rev-parse --short HEAD) || return 1
	printf 'finix %s\n' "$rev" >>"$log"
	FINIX_REV=$rev
	printf '%s\n' "$rev" >"$ROOT/switch-pending"
	return 0
}

push_landing() {
	local id=$1 repo=$2 tip=$3 base=$4 ref=$5 log=$6 head
	: >"$log"
	head=$(git -C "$repo" rev-parse --verify --quiet refs/remotes/origin/main) || true
	if [[ $head != "$base" ]]; then
		printf 'origin/main moved to %s since %s; not pushing\n' "$head" "$base" >>"$log"
		return 1
	fi
	git -C "$repo" push origin "$tip:refs/heads/main" >>"$log" 2>&1 || return 1
	head=$(git ls-remote --heads origin main | cut -f1)
	if [[ $head != "$tip" ]]; then
		printf 'origin main is %s, not %s\n' "$head" "$tip" >>"$log"
		return 1
	fi
	if [[ $ref =~ ^[0-9a-f]{7,40}$ ]]; then
		printf 'pushed %s by commit id\n' "$tip" >>"$log"
	else
		git -C "$repo" push origin --delete "$ref" >>"$log" 2>&1 || true
		printf 'pushed %s and deleted %s\n' "$tip" "$ref" >>"$log"
	fi
	if git -C "$repo" push forgejo "$tip:refs/heads/main" >"$log.forgejo" 2>&1 &&
		[[ $(git ls-remote --heads forgejo main | cut -f1) == "$tip" ]]; then
		printf 'forgejo has %s\n' "$tip" >>"$log.forgejo"
		return 0
	fi
	printf 'forgejo did not take %s; push it again when the server answers\n' "$tip" \
		>>"$log.forgejo"
	return 2
}

run_timing() {
	local id=$1 suite=$2 limit=$3 ref=$4
	local log=$LOGS/$id rc=0 repo tree driver pid sha
	mkdir -p "$log"
	repo=$(repo_root) || {
		finish "$id" failed "" "" resolve "$log/resolve.log"
		return 1
	}
	git -C "$repo" fetch --quiet origin >/dev/null 2>&1 || true
	sha=$(git -C "$repo" rev-parse --verify --quiet "refs/remotes/origin/$ref^{commit}")
	[[ -n $sha ]] || sha=$(git -C "$repo" rev-parse --verify --quiet "$ref^{commit}")
	[[ -n $sha ]] || {
		printf '%s is not a commit here\n' "$ref" >"$log/resolve.log"
		finish "$id" failed "" "" resolve "$log/resolve.log"
		return 1
	}
	printf 'ref %s\nsha %s\n' "$ref" "$sha" >"$log/resolve.log"
	tree=$ROOT/work/$id
	rm -rf "$tree"
	git -C "$repo" worktree prune >/dev/null 2>&1 || true
	git -C "$repo" worktree add --quiet --detach "$tree" "$sha" >>"$log/resolve.log" 2>&1 || {
		finish "$id" failed "" "" resolve "$log/resolve.log"
		return 1
	}
	if ! take_lease -s; then
		cleanup_tree "$repo" "$tree"
		finish "$id" failed "$sha" "" lease "$log/timing.log"
		return 75
	fi
	driver=$(
		cd "$tree" || exit 1
		nice -n 10 nix build --no-link --print-out-paths --max-jobs 1 \
			--option cores "$NIXCORES" ".#legacyPackages.$SYSTEM.suites.$suite.driver"
	) >"$log/timing.log" 2>&1
	release_lease
	[[ -f $RESULTS/$id ]] && {
		printf 'land: cancelled before the run\n' >>"$log/timing.log"
		cleanup_tree "$repo" "$tree"
		return 75
	}
	if [[ -z $driver ]]; then
		cleanup_tree "$repo" "$tree"
		finish "$id" failed "$sha" "" driver "$log/timing.log"
		return 1
	fi
	if ! take_lease -x; then
		cleanup_tree "$repo" "$tree"
		finish "$id" failed "$sha" "" lease "$log/timing.log"
		return 75
	fi
	mkdir -p "$ROOT/run/$id/out"
	set -m
	timeout --signal=TERM --kill-after="$KILLWAIT" "$limit" \
		bash -c 'exec 9>&-; cd "$1" && LOGFILE=/dev/null nice -n 10 "$2/bin/nixos-test-driver" -o "$1/out"' _ \
		"$ROOT/run/$id" "$driver" >>"$log/timing.log" 2>&1 &
	pid=$!
	set +m
	printf '%s\n' "$pid" >"$ACTIVE_FILE.pid"
	wait "$pid" || rc=$?
	rm -f "$ACTIVE_FILE.pid"
	release_lease
	cleanup_tree "$repo" "$tree"
	case $rc in
	124 | 137)
		printf 'land: %s hit its %ss limit\n' "$suite" "$limit" >>"$log/timing.log"
		finish "$id" failed "$sha" "" timing "$log/timing.log"
		return 1
		;;
	esac
	if grep -q 'tomoe suite passed:' "$log/timing.log"; then
		finish "$id" passed "$sha" "$sha" timing "$log/timing.log"
		return 0
	fi
	finish "$id" failed "$sha" "" timing "$log/timing.log"
	return 1
}

finish() {
	local id=$1 status=$2 sha=$3 main=$4 step=$5 log=$6 tmp=$RESULTS/.$1.tmp
	[[ -f $RESULTS/$id ]] && return 0
	{
		printf '{"id":"%s","status":"%s","sha":"%s","main":"%s","steps":[' \
			"$id" "$status" "$sha" "$main"
		local first=1 name result path
		while read -r name result path; do
			((first)) || printf ','
			first=0
			printf '{"step":"%s","result":"%s","log":"%s"}' "$name" "$result" "$path"
		done <"$ROOT/steps"
		[[ -n $step ]] && {
			((first)) || printf ','
			printf '{"step":"%s","result":"%s","log":"%s"}' \
				"$step" "$([[ $status == passed ]] && echo passed || echo failed)" "$log"
		}
		printf '],"finix":"%s","finished":"%s"}\n' "${FINIX_REV:-}" "$(date -u +%FT%TZ)"
	} >"$tmp"
	mv "$tmp" "$RESULTS/$id"
	printf '%s %s %s\n' "$id" "$status" "$(date -u +%FT%TZ)" >>"$ROOT/results.log"
}

finix_pin_rev() {
	if command -v jq >/dev/null 2>&1; then
		jq -r '.nodes.tomoe.locked.rev // empty' "$FINIX/flake.lock" 2>/dev/null
	elif command -v python3 >/dev/null 2>&1; then
		python3 -c 'import json,sys
d = json.load(open(sys.argv[1]))
print(d["nodes"]["tomoe"]["locked"].get("rev", ""))' "$FINIX/flake.lock" 2>/dev/null
	fi
}

retry_pin() {
	local sha pinned to now
	sha=$(cat "$ROOT/undeployed" 2>/dev/null)
	[[ -n $sha ]] || {
		rm -f "$ROOT/undeployed"
		return 0
	}
	if [[ ! -f $ROOT/switch-pending ]]; then
		pinned=$(finix_pin_rev)
		to=$(sed -n 's/.*"to":"\([^"]*\)".*/\1/p' "$ROOT/switch.log" 2>/dev/null | tail -1)
		now=$(readlink -f /run/current-system 2>/dev/null)
		if [[ -n $pinned && -n $to && $to == "$now" ]] &&
			git -C "$REPO" merge-base --is-ancestor "$sha" "$pinned" 2>/dev/null; then
			rm -f "$ROOT/undeployed"
			printf '%s deployed %s\n' "$(date -u +%FT%TZ)" "$sha" >>"$ROOT/journal.log"
			return 0
		fi
	fi
	pin_finix "$ROOT/pin-retry.log" "$sha" || true
	return 0
}

job_main() {
	local ticket=$1 next id type ref suite timeout
	next=$(basename "$ticket")
	id=${next#*-}
	ACTIVE_FILE=$ticket
	: >"$ROOT/steps"
	if [[ -f $RESULTS/$id ]]; then
		mv "$ticket" "$DONE/$next" 2>/dev/null || rm -f "$ticket"
		return 0
	fi
	IFS=$'\t' read -r type ref suite timeout <"$ticket"
	printf '%s start %s %s %s\n' "$(date -u +%FT%TZ)" "$id" "$type" "$ref" >>"$ROOT/daemon.log"
	printf '%s taken %s\n' "$(date -u +%FT%TZ)" "$id" >>"$ROOT/journal.log"
	case $type in
	land) run_land "$id" "$ref" ;;
	timing)
		[[ -n $timeout ]] || timeout=$TIMING_TIMEOUT
		[[ -n $suite ]] || suite=timing
		run_timing "$id" "$suite" "$timeout" "$ref"
		;;
	*) finish "$id" failed "" "" type "" ;;
	esac
	if [[ ! -f $RESULTS/$id ]]; then
		finish "$id" failed "" "" lost "no result was written"
		printf '%s lost %s\n' "$(date -u +%FT%TZ)" "$id" >>"$ROOT/journal.log"
	fi
	mv "$ticket" "$DONE/$next" 2>/dev/null || rm -f "$ticket"
	rm -f "$ticket.pid"
	printf '%s done %s\n' "$(date -u +%FT%TZ)" "$id" >>"$ROOT/daemon.log"
}

daemon() {
	mkdir -p "$ROOT"
	rm -f "$PENDING/$AGENT."* 2>/dev/null
	for leftover in "$ACTIVE"/*; do
		[[ -e $leftover ]] || continue
		case $leftover in
		*.pid)
			rm -f "$leftover"
			continue
			;;
		esac
		if [[ -f $leftover.pid ]] && kill -0 "$(cat "$leftover.pid")" 2>/dev/null; then
			printf '%s adopting %s\n' "$(date -u +%FT%TZ)" "$(basename "$leftover")" \
				>>"$ROOT/journal.log"
			continue
		fi
		mv "$leftover" "$QUEUE/" 2>/dev/null || true
		printf '%s requeued %s\n' "$(date -u +%FT%TZ)" "$(basename "$leftover")" \
			>>"$ROOT/journal.log"
	done
	printf '%s daemon up pid %s\n' "$(date -u +%FT%TZ)" "$$" >>"$ROOT/daemon.log"
	[[ -f $ROOT/undeployed ]] && retry_pin
	while :; do
		local next id supervisor sp busy=""
		for adopted in "$ACTIVE"/*; do
			[[ -e $adopted ]] || continue
			case $adopted in *.pid) continue ;; esac
			if [[ -f $RESULTS/${adopted##*-} ]]; then
				mv "$adopted" "$DONE/$(basename "$adopted")" 2>/dev/null || rm -f "$adopted"
				rm -f "$adopted.pid"
				continue
			fi
			sp=$(cat "$adopted.pid" 2>/dev/null)
			if [[ -n $sp ]] && kill -0 "$sp" 2>/dev/null; then
				busy=$adopted
				break
			fi
			if mv "$adopted" "$QUEUE/$(basename "$adopted")" 2>/dev/null; then
				rm -f "$adopted.pid"
				printf '%s requeued %s (no supervisor)\n' "$(date -u +%FT%TZ)" \
					"$(basename "$adopted")" >>"$ROOT/journal.log"
			fi
		done
		if [[ -n $busy ]]; then
			sleep 2
			continue
		fi
		next=$(for f in $(ls "$QUEUE" 2>/dev/null | grep -v -e '\.tmp$' -e '\.pid$'); do
			kind=$(cut -f1 "$QUEUE/$f" 2>/dev/null)
			if [[ $kind == land ]]; then printf '0 %s\n' "$f"; else printf '1 %s\n' "$f"; fi
		done | sort | head -1 | cut -d' ' -f2-)
		if [[ -z $next ]]; then
			if [[ -f $ROOT/undeployed ]]; then
				retry_pin
			fi
			if [[ -f $ROOT/switch-pending ]]; then
				local rev
				rev=$(cat "$ROOT/switch-pending")
				rm -f "$ROOT/switch-pending"
				printf '%s switching finix %s\n' "$(date -u +%FT%TZ)" "$rev" \
					>>"$ROOT/journal.log"
				(
					cd "$FINIX" || exit 1
					setsid nohup finix-switch >>"$ROOT/switch.log" 2>&1 &
				)
				sleep 30
			fi
			sleep 1
			continue
		fi
		ACTIVE_FILE=$ACTIVE/$next
		mv "$QUEUE/$next" "$ACTIVE_FILE" 2>/dev/null || continue
		id=${next#*-}
		if [[ -f $RESULTS/$id ]]; then
			mv "$ACTIVE_FILE" "$DONE/$next" 2>/dev/null || rm -f "$ACTIVE_FILE"
			continue
		fi
		setsid "$0" _job "$ACTIVE_FILE" >>"$ROOT/jobs.log" 2>&1 &
		supervisor=$!
		printf '%s\n' "$supervisor" >"$ACTIVE_FILE.pid"
		while [[ ! -f $RESULTS/$id ]] && kill -0 "$supervisor" 2>/dev/null; do
			sleep 2
		done
	done
}

case ${1:-} in
daemon) shift; daemon ;;
_job) shift; job_main "$@" ;;
submit) shift; submit "$@" ;;
wait) shift; wait_result "$@" ;;
status) shift; status ;;
cancel) shift; cancel "$@" ;;
-h | --help | help) usage ;;
*) usage >&2; exit 2 ;;
esac
