#!/usr/bin/env bash
set -euo pipefail
PATH=$SPEEDLAB_PATH

if [[ ${1:-} != --inside ]]; then
	run=$1 persona=$2 offset=$3
	shift 3
	exec unshare --user --map-root-user --mount --ipc --uts --pid --fork --kill-child --mount-proc \
		--time --monotonic "$offset" --boottime "$offset" -- \
		"$SPEEDLAB_BASH" "$0" --inside "$run" "$persona" "$@"
fi
shift
run=$1 persona=$2
shift 2
stage=$run/stage
root=$run/root
account=$(<"$stage/account")
uid=$(<"$stage/uid")
gid=$(<"$stage/gid")
home=$(<"$stage/home-path")
node=$(<"$stage/render-node")

mount --make-rprivate /
mkdir -p "$root"
mount -t tmpfs -o mode=0755 speedlab "$root"
mkdir -p "$root"/{nix,proc,sys,dev,etc,tmp,run,var/tmp,var/cache,bin,usr/bin,"${home#/}"}
mount --rbind -o ro=recursive /nix "$root/nix"
mount --rbind -o ro=recursive /sys "$root/sys"
mount -t proc proc "$root/proc"

mount -t tmpfs -o mode=0755 speedlab "$root/dev"
for name in null zero full random urandom tty; do
	: >"$root/dev/$name"
	mount --bind "/dev/$name" "$root/dev/$name"
done
mkdir -p "$root/dev/dri" "$root/dev/shm" "$root/dev/pts"
: >"$root/dev/dri/${node##*/}"
mount --bind "$node" "$root/dev/dri/${node##*/}"
mount -t tmpfs -o mode=1777 speedlab "$root/dev/shm"
ln -s /proc/self/fd "$root/dev/fd"
ln -s /proc/self/fd/0 "$root/dev/stdin"
ln -s /proc/self/fd/1 "$root/dev/stdout"
ln -s /proc/self/fd/2 "$root/dev/stderr"

mount -t tmpfs -o mode=0755 speedlab "$root/etc"
for entry in /etc/*; do
	name=${entry##*/}
	case $name in ld.so.preload | ld.so.cache | mtab) continue ;; esac
	if [[ -L $entry ]]; then
		ln -s "$(readlink "$entry")" "$root/etc/$name"
	elif [[ -d $entry ]]; then
		mkdir "$root/etc/$name"
		mount --rbind -o ro=recursive "$entry" "$root/etc/$name" 2>/dev/null || rmdir "$root/etc/$name"
	elif [[ -f $entry && -r $entry ]]; then
		cp "$entry" "$root/etc/$name"
	fi
done
ln -s /proc/self/mounts "$root/etc/mtab"

mount -t tmpfs -o mode=0755 speedlab "$root/run"
mkdir -p "$root/run/user/$uid" "$root/run/speedlab/stage"
chmod 0700 "$root/run/user/$uid"
mount --rbind -o ro=recursive "$stage" "$root/run/speedlab/stage"
for link in current-system booted-system; do
	[[ -e /run/$link ]] && ln -s "$(readlink -f "/run/$link")" "$root/run/$link"
done
mount -t tmpfs -o mode=1777 speedlab "$root/tmp"
mount -t tmpfs -o mode=1777 speedlab "$root/var/tmp"
if [[ -d /var/cache/fontconfig ]]; then
	mkdir -p "$root/var/cache/fontconfig"
	mount --rbind -o ro=recursive /var/cache/fontconfig "$root/var/cache/fontconfig"
fi
ln -s "$SPEEDLAB_BASH" "$root/bin/sh"
ln -s "$(realpath "$(command -v env)")" "$root/usr/bin/env"

mount -t tmpfs -o mode=0700 speedlab "$root/$home"
cp -a "$stage/home/." "$root/$home/"

cd "$root"
mkdir .old
pivot_root . .old
umount -l /.old
rmdir /.old
cd /
exec unshare --user --map-user="$uid" --map-group="$gid" -- \
	env -i "PATH=$SPEEDLAB_PATH" "SPEEDLAB_TOOLS=$SPEEDLAB_TOOLS" "SPEEDLAB_MESA=$SPEEDLAB_MESA" \
	"SPEEDLAB_PERF=$SPEEDLAB_PERF" "SPEEDLAB_PERSONA=$persona" "SPEEDLAB_HOME=$home" "SPEEDLAB_ACCOUNT=$account" "SPEEDLAB_UID=$uid" \
	"$SPEEDLAB_TOOLS/bin/speedlab-driver" "$@"
