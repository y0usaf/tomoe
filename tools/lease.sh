declare -gA LEASE_HOLDERS=()
declare -gA LEASE_ACKS=()
LEASE_HOLDER=""
LEASE_ACK=""

lease_usage() {
	printf 'usage: lease_take LOCK -s|-x [SECONDS]   (0 or none waits for it)\n       lease_release [LOCK]\n' >&2
}

lease_key() {
	realpath -m "$1" 2>/dev/null || printf '%s' "$1"
}

lease_sync_latest() {
	local key
	LEASE_HOLDER=""
	LEASE_ACK=""
	if ((${#LEASE_HOLDERS[@]} > 0)); then
		for key in "${!LEASE_HOLDERS[@]}"; do
			LEASE_HOLDER=${LEASE_HOLDERS[$key]}
			LEASE_ACK=${LEASE_ACKS[$key]:-}
		done
	fi
}

lease_take() {
	local lock=${1:-} mode=${2:-} wait=${3:-0} rc=0 tick=0 logged="" caller=$BASHPID
	local key ack pid
	local -a holder
	if [[ -z $lock || ( $mode != -s && $mode != -x ) || ! $wait =~ ^[0-9]+$ ]]; then
		lease_usage
		return 2
	fi
	key=$(lease_key "$lock")
	if [[ -n ${LEASE_HOLDERS[$key]:-} ]]; then
		if kill -0 "${LEASE_HOLDERS[$key]}" 2>/dev/null; then
			printf 'lease_take: already holding %s\n' "$lock" >&2
			return 2
		fi
		lease_release "$lock"
	fi
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
	pid=$!
	while [[ ! -s $ack ]]; do
		if ! kill -0 "$pid" 2>/dev/null; then
			if wait "$pid" 2>/dev/null; then
				rc=0
			else
				rc=$?
			fi
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
	LEASE_HOLDERS[$key]=$pid
	LEASE_ACKS[$key]=$ack
	LEASE_HOLDER=$pid
	LEASE_ACK=$ack
	return 0
}

lease_release() {
	local key pid ack
	if (($# > 0)) && [[ -n ${1:-} ]]; then
		key=$(lease_key "$1")
		pid=${LEASE_HOLDERS[$key]:-}
		ack=${LEASE_ACKS[$key]:-}
		if [[ -n $pid ]]; then
			kill -TERM "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
			unset 'LEASE_HOLDERS[$key]'
		fi
		if [[ -n $ack ]]; then
			rm -f "$ack"
			unset 'LEASE_ACKS[$key]'
		fi
		lease_sync_latest
		return 0
	fi
	if ((${#LEASE_HOLDERS[@]} > 0)); then
		for key in "${!LEASE_HOLDERS[@]}"; do
			pid=${LEASE_HOLDERS[$key]:-}
			if [[ -n $pid ]]; then
				kill -TERM "$pid" 2>/dev/null || true
				wait "$pid" 2>/dev/null || true
			fi
		done
	fi
	if ((${#LEASE_ACKS[@]} > 0)); then
		for key in "${!LEASE_ACKS[@]}"; do
			ack=${LEASE_ACKS[$key]:-}
			if [[ -n $ack ]]; then
				rm -f "$ack"
			fi
		done
	fi
	LEASE_HOLDERS=()
	LEASE_ACKS=()
	LEASE_HOLDER=""
	LEASE_ACK=""
	return 0
}
