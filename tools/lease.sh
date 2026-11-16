LEASE_HOLDER=""
LEASE_ACK=""

lease_usage() {
	printf 'usage: lease_take LOCK -s|-x [SECONDS]   (0 or none waits for it)\n       lease_release\n' >&2
}

lease_take() {
	local lock=${1:-} mode=${2:-} wait=${3:-0} rc=0 tick=0 logged="" caller=$BASHPID
	local -a holder
	if [[ -z $lock || ( $mode != -s && $mode != -x ) ]]; then
		lease_usage
		return 2
	fi
	lease_release
	mkdir -p "$(dirname "$lock")" || return 1
	LEASE_ACK=$(mktemp "${TMPDIR:-/tmp}/tomoe-lease.XXXXXX") || return 1
	holder=(flock "$mode" -o -E 75)
	((wait > 0)) && holder+=(-w "$wait")
	holder+=("$lock" bash -c '
		printf lease >"$1" || exit 1
		while kill -0 "$PPID" 2>/dev/null && kill -0 "$2" 2>/dev/null; do sleep 1; done
	' _ "$LEASE_ACK" "$caller")
	"${holder[@]}" </dev/null >/dev/null 2>&1 &
	LEASE_HOLDER=$!
	while [[ ! -s $LEASE_ACK ]]; do
		if ! kill -0 "$LEASE_HOLDER" 2>/dev/null; then
			wait "$LEASE_HOLDER" 2>/dev/null
			rc=$?
			LEASE_HOLDER=""
			rm -f "$LEASE_ACK"
			LEASE_ACK=""
			return $((rc == 0 ? 1 : rc))
		fi
		if [[ -n ${LEASE_LOG:-} && -z $logged ]] && ((tick > 1500)); then
			logged=1
			{
				printf '%s lease-wait %s still blocked on %s\n' \
					"$(date -u +%FT%TZ)" "$mode" "$lock"
				fuser -v "$lock" 2>&1
				grep ":$({ stat -c '%i' "$lock"; } 2>/dev/null) " /proc/locks 2>/dev/null
			} >>"$LEASE_LOG" 2>&1
		fi
		if ((tick < 20)); then sleep 0.05; else sleep 1; fi
		tick=$((tick + 1))
	done
	return 0
}

lease_release() {
	[[ -n $LEASE_HOLDER ]] || return 0
	kill -TERM "$LEASE_HOLDER" 2>/dev/null
	wait "$LEASE_HOLDER" 2>/dev/null
	LEASE_HOLDER=""
	[[ -n $LEASE_ACK ]] && rm -f "$LEASE_ACK"
	LEASE_ACK=""
	return 0
}
