#!/usr/bin/env bash
set -uo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lib=${LEASE_LIB:-$here/../tools/lease.sh}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
lock=$work/tomoe-vm.lock
fail() {
	printf 'lease: FAIL: %s\n' "$*" >&2
	exit 1
}
free() { flock -n -x "$lock" true; }

cat >"$work/taker" <<EOF
#!/usr/bin/env bash
set -uo pipefail
. "$lib"
lease_take "\$1" -x 5 || exit 75
sleep 300 &
printf '%s\n' \$! >"\$2"
: >"\$3"
wait
EOF
bash "$work/taker" "$lock" "$work/child" "$work/ready" &
taker=$!
for _ in $(seq 100); do [[ -f $work/ready ]] && break; sleep 0.1; done
[[ -f $work/ready ]] || fail "the taker never held the lease"
free && fail "the lease is free while the taker still holds it"
child=$(cat "$work/child")
kill -KILL "$taker" 2>/dev/null
wait "$taker" 2>/dev/null
kill -0 "$child" 2>/dev/null || fail "the child that should outlive the taker is gone"
released=""
for _ in $(seq 100); do free && { released=yes; break; }; sleep 0.1; done
kill -KILL "$child" 2>/dev/null
[[ -n $released ]] || fail "a child that outlived the process that took the lease still holds it"

flock -x "$lock" sleep 0.5 &
holder=$!
sleep 0.2
( . "$lib"; lease_take "$lock" -x 0 && lease_release ) ||
	fail "a zero wait failed instead of waiting for the lease"
wait "$holder"
flock -x "$lock" sleep 2 &
holder=$!
sleep 0.2
( . "$lib"; lease_take "$lock" -x 1 ) &&
	fail "a lease was taken while another process held it"
wait "$holder"

. "$lib"
lease_take "$lock" -x 5 || fail "the test could not take the lease"
for fd in "/proc/$$/fd/"*; do
	[[ $(readlink "$fd" 2>/dev/null) == "$lock" ]] &&
		fail "the shell that took the lease holds a descriptor for it ($fd)"
done
lease_release
free || fail "the lease is still held after it was released"

cat >"$work/strict" <<EOF
#!/usr/bin/env bash
set -euo pipefail
. "$lib"
lease_take "\$1" -s 0
lease_release
lease_take "\$1" -x 0
kill -KILL "\$LEASE_HOLDER" 2>/dev/null
sleep 0.2
lease_release
EOF
bash "$work/strict" "$lock" 2>/dev/null ||
	fail "a shell running with set -e cannot take and release the lease"

lock2=$work/tomoe-switch.lock
free2() { flock -n -x "$lock2" true; }
lease_take "$lock2" -x 5 || fail "could not take second lock"
lease_take "$lock" -s 5 || fail "could not take nested lock while holding another"
free2 && fail "taking a second lock dropped the first lock"
free && fail "nested lock is not held"
if lease_take "$lock2" -x 1 2>/dev/null; then
	fail "retaking an already-held lock succeeded"
fi
free2 && fail "refused retake dropped the held lock"
lease_release "$lock"
free || fail "releasing nested lock by name did not free it"
free2 && fail "releasing nested lock by name dropped the outer lock"
lease_release "$lock2"
free2 || fail "releasing outer lock by name did not free it"

printf 'lease: held only by the process that needs it, and released with it\n'
