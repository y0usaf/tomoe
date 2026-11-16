declare -A LEASE_HOLDER=()
declare -A LEASE_ACK=()

lease_usage() {
	printf 'usage: lease_take LOCK -s|-x [SECONDS]   (0 or none waits for it)\n       lease_release [LOCK]                (no LOCK releases every one held)\n       lease_holder LOCK\n' >&2
}

lease_holder() {
	local lock=${1:-}
	[[ -n ${LEASE_HOLDER[$lock]:-} ]] || return 1
	printf '%s\n' "${LEASE_HOLDER[$lock]}"
}

lease_take() {
	local lock=${1:-} mode=${2:-} wait=${3:-0} rc=0 tick=0 logged="" caller=$BASHPID ack
	local -a holder
	if [[ -z $lock || ( $mode != -s && $mode != -x ) ]]; then
		lease_usage
		return 2
	fi
	lease_release "$lock"
	mkdir -p "$(dirname "$lock")" || return 1
	ack=$(mktemp "${TMPDIR:-/tmp}/tomoe-lease.XXXXXX") || return 1
	holder=(flock "$mode" -o -E 75)
	if ((wait > 0)); then
		holder+=(-w "$wait")
	fi
	holder+=("$lock" bash -c '
		printf lease >"$1" || exit 1
		while kill -0 "$PPID" 2>/dev/null && kill -0 "$2" 2>/dev/null; do sleep 1; done
	' _ "$ack" "$caller")
	"${holder[@]}" </dev/null >/dev/null 2>&1 &
	LEASE_HOLDER[$lock]=$!
	LEASE_ACK[$lock]=$ack
	while [[ ! -s $ack ]]; do
		if ! kill -0 "${LEASE_HOLDER[$lock]}" 2>/dev/null; then
			if wait "${LEASE_HOLDER[$lock]}" 2>/dev/null; then
				rc=0
			else
				rc=$?
			fi
			unset 'LEASE_HOLDER[$lock]' 'LEASE_ACK[$lock]'
			rm -f "$ack"
			return $((rc == 0 ? 1 : rc))
		fi
		if [[ -n ${LEASE_LOG:-} && -z $logged ]] && ((tick > 1500)); then
			logged=1
			{
				printf '%s lease-wait %s still blocked on %s\n' \
					"$(date -u +%FT%TZ)" "$mode" "$lock"
				fuser -v "$lock" 2>&1
				grep ":$({ stat -c '%i' "$lock"; } 2>/dev/null) " /proc/locks 2>/dev/null
			} >>"$LEASE_LOG" 2>&1 || true
		fi
		if ((tick < 20)); then sleep 0.05; else sleep 1; fi
		tick=$((tick + 1))
	done
	return 0
}

lease_release() {
	local one
	local -a locks
	if [[ -n ${1:-} ]]; then
		locks=("$1")
	else
		locks=("${!LEASE_HOLDER[@]}")
	fi
	for one in "${locks[@]}"; do
		[[ -n ${LEASE_HOLDER[$one]:-} ]] || continue
		kill -TERM "${LEASE_HOLDER[$one]}" 2>/dev/null || true
		wait "${LEASE_HOLDER[$one]}" 2>/dev/null || true
		if [[ -n ${LEASE_ACK[$one]:-} ]]; then
			rm -f "${LEASE_ACK[$one]}"
		fi
		unset 'LEASE_HOLDER[$one]' 'LEASE_ACK[$one]'
	done
	return 0
}
