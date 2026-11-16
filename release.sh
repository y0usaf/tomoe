fail() {
  echo "release: $*" >&2
  exit 1
}

usage() {
  cat >&2 <<'EOF'
usage: nix run .#release -- rc VERSION [REV] [--dry-run]
       nix run .#release -- soak TAG [--dry-run]
       nix run .#release -- promote TAG [--dry-run]

A release goes main -> candidate -> soak -> stable, run from a clean checkout:

  rc       tags REV (default origin/main) vVERSION-rc.N, the next N. REV must be
           on origin/main and its flake.nix must already declare VERSION, with a
           "## VERSION" section in CHANGELOG.md; land that commit first.
  soak     runs the candidate's own gate.sh on a private headless instance with
           the config finix generates for it, then keeps it busy for
           RELEASE_SOAK seconds (default 1800). The result, with the finix
           revision whose config it ran, is recorded as a git note on the commit
           (refs/notes/soak). Any failed soak rules the candidate out.
  promote  tags the newest candidate's commit vVERSION once it has a passing
           soak of at least RELEASE_SOAK seconds and no failed one, and moves
           tomoe-stable in finix to that commit with the finix revision it was
           soaked with.

Without --dry-run, rc and promote push the tag and publish a GitHub (pre)release,
soak pushes its note, and promote commits and pushes finix. With --dry-run tags
live under refs/dry-run/tags and notes under refs/notes/soak-dry-run, nothing is
pushed or published, and the finix change is made, locked and built in a
throwaway worktree, then shown.

  RELEASE_FINIX  finix checkout (default ~/finix)
  RELEASE_SOAK   seconds a soak runs and promote requires (default 1800)
EOF
  exit 2
}

dry=false
args=()
for arg in "$@"; do
  if [[ $arg == --dry-run ]]; then dry=true; else args+=("$arg"); fi
done
set -- "${args[@]}"
(($# >= 2)) || usage
command=$1
FINIX=${RELEASE_FINIX:-$HOME/finix}
SOAK=${RELEASE_SOAK:-1800}
LOCK=${TOMOE_LOCK:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/agent/tomoe-vm.lock}
SWITCH_LOCK=${TOMOE_SWITCH_LOCK:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/agent/finix-switch.lock}
LEASE_LIB=${TOMOE_LEASE_LIB:-$(dirname "$(realpath "${BASH_SOURCE[0]}")")/tools/lease.sh}
# shellcheck source=tools/lease.sh
. "$LEASE_LIB"
SYSTEM=$(nix eval --raw --impure --expr builtins.currentSystem)
[[ $SOAK =~ ^[0-9]+$ ]] || fail "RELEASE_SOAK must be a number of seconds"
$dry || ((SOAK >= 1800)) || fail "a release soaks at least 1800s; a shorter RELEASE_SOAK is for --dry-run"
cd "$(git rev-parse --show-toplevel)" || exit
REPO=$PWD
if $dry; then
  TAGS=refs/dry-run/tags NOTES=refs/notes/soak-dry-run flag=" --dry-run"
else
  TAGS=refs/tags NOTES=refs/notes/soak flag=""
fi

tag_rev() { git rev-parse -q --verify "$TAGS/$1^{commit}"; }

candidates() {
  {
    git for-each-ref --format='%(refname:lstrip=-1)' "$TAGS/v$1-rc.*"
    $dry || git ls-remote --tags --refs origin "v$1-rc.*" | sed 's#.*refs/tags/##'
  } | sed -n "s/^v$1-rc\.\([0-9][0-9]*\)$/\1/p" | sort -n | uniq
}

released() {
  tag_rev "v$1" >/dev/null && return 0
  ! $dry && git ls-remote --exit-code --tags origin "refs/tags/v$1" >/dev/null
}

changelog() {
  git show "$2:CHANGELOG.md" 2>/dev/null |
    awk -v heading="## $1" '$0 == heading { found = 1; next } /^## / && found { exit } found'
}

parse_candidate() {
  [[ $1 =~ ^v([0-9]+\.[0-9]+\.[0-9]+)-rc\.([0-9]+)$ ]] || fail "$1 is not a candidate tag (vX.Y.Z-rc.N)"
  version=${BASH_REMATCH[1]} number=${BASH_REMATCH[2]}
  rev=$(tag_rev "$1") || fail "no tag $1 under $TAGS"
}

cut_candidate() {
  version=$1
  [[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "VERSION is X.Y.Z"
  git fetch -q origin main
  rev=$(git rev-parse --verify "${2:-origin/main}^{commit}") || fail "no commit ${2:-origin/main}"
  git merge-base --is-ancestor "$rev" origin/main || fail "$rev is not on origin/main"
  declared=$(git show "$rev:flake.nix" | sed -n 's/^ *version = "\(.*\)";$/\1/p')
  [[ $declared == "$version" ]] ||
    fail "flake.nix at $rev declares version \"$declared\"; land a commit that sets \"$version\" first"
  notes=$(changelog "$version" "$rev")
  [[ -n ${notes//[[:space:]]/} ]] || fail "CHANGELOG.md at $rev has no '## $version' section"
  released "$version" && fail "v$version is already released"
  last=$(candidates "$version" | tail -1)
  if [[ -n $last ]] && [[ $(tag_rev "v$version-rc.$last" 2>/dev/null) == "$rev" ]]; then
    fail "v$version-rc.$last is already $rev"
  fi
  tag=v$version-rc.$((${last:-0} + 1))
  lease_take "$LOCK" -s 0 || fail "cannot take $LOCK"
  out=$(nix build --no-link --print-out-paths "git+file://$REPO?rev=$rev")
  lease_release
  [[ $("$out/bin/tomoe" --version) == "tomoe $version,"* ]] || fail "$out/bin/tomoe --version does not report $version"
  if $dry; then
    git update-ref "$TAGS/$tag" "$rev"
  else
    git tag -a "$tag" "$rev" -m "Tomoe $version release candidate ${tag##*-rc.}"
    git push -q origin "refs/tags/$tag"
    repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
    printf 'Release candidate %s for Tomoe %s. Try it with\n\n    nix run github:%s/%s\n\nand report problems at https://github.com/%s/issues.\n\n%s\n' \
      "${tag##*-rc.}" "$version" "$repo" "$tag" "$repo" "$notes" |
      gh release create "$tag" --verify-tag --prerelease --title "Tomoe $version-rc.${tag##*-rc.}" --notes-file -
  fi
  echo "release: $tag is $rev ($out)"
  echo "release: next, soak it: nix run .#release -- soak $tag$flag"
}

soak_candidate() {
  parse_candidate "$1"
  finix=$(git -C "$FINIX" rev-parse --verify HEAD) || fail "no finix checkout at $FINIX"
  [[ -n $(git -C "$FINIX" branch -r --contains "$finix") ]] ||
    fail "finix $finix is on no remote branch; tomoe-stable can only pin a pushed revision"
  gate=$(mktemp -d)
  log=$gate/soak.log
  git archive "$rev" gate.sh tools/lease.sh tools/isolate | tar -x -C "$gate" ||
    fail "$1 has no gate.sh, tools/lease.sh and tools/isolate"
  echo "release: soaking $1 ($rev) with finix $finix for ${SOAK}s"
  started=$(date -u +%FT%TZ)
  if GATE_FINIX=$FINIX GATE_FINIX_REV=$finix GATE_SOAK=$SOAK "$gate/gate.sh" "$rev" 2>&1 | tee "$log"; then
    result=PASS
  else
    result=FAIL
  fi
  package=$(sed -n 's/^tomoe-gate: PASS [0-9a-f]* //p' "$log" | tail -1)
  git notes --ref "$NOTES" append -m \
    "soak $result tag=$1 finix=$finix seconds=$SOAK started=$started finished=$(date -u +%FT%TZ) package=${package:-none}" "$rev"
  $dry || git push -q origin "$NOTES"
  rm -rf "$gate"
  [[ $result == PASS ]] || fail "$1 failed its soak (recorded in $NOTES); fix it and cut the next candidate"
  echo "release: $1 soaked ${SOAK}s with no failure; next: nix run .#release -- promote $1$flag"
}

move_stable() {
  local tag=$1 finix=$2 tomoe=$3 tree commit stable
  if $dry; then
    tree=$(mktemp -d)/finix
    git -C "$FINIX" worktree add -q --detach "$tree" HEAD
  else
    tree=$FINIX
    git -C "$tree" diff --quiet HEAD -- flake.nix flake.lock ||
      fail "$tree has uncommitted flake.nix or flake.lock changes; not touching them"
    git -C "$tree" diff --quiet --cached || fail "$tree has staged changes; not touching its index"
    lease_take "$SWITCH_LOCK" -x 0 || fail "cannot take $SWITCH_LOCK"
  fi
  [[ $(grep -c '^ *tomoe-stable\.url = "github:[^/]*/[^/]*/[0-9a-f]\{40\}";$' "$tree/flake.nix") == 1 &&
    $(grep -c '^ *tomoe-stable\.inputs\.tomoe\.url = "github:[^/]*/[^/]*/[0-9a-f]\{40\}";$' "$tree/flake.nix") == 1 ]] ||
    fail "$tree/flake.nix has no single pinned tomoe-stable.url and tomoe-stable.inputs.tomoe.url"
  sed -i -e "s#^\( *tomoe-stable\.url = \"github:[^/]*/[^/]*/\)[0-9a-f]\{40\}\";\$#\1$finix\";#" \
    -e "s#^\( *tomoe-stable\.inputs\.tomoe\.url = \"github:[^/]*/[^/]*/\)[0-9a-f]\{40\}\";\$#\1$tomoe\";#" "$tree/flake.nix"
  (cd "$tree" && nix flake update tomoe-stable) || fail "cannot lock tomoe-stable at finix $finix, tomoe $tomoe"
  git -C "$tree" -c user.name=tomoe-release -c user.email=tomoe-release@localhost commit -q \
    -m "tomoe-stable: $tag (${tomoe:0:7}) with finix ${finix:0:8}'s config" -- flake.nix flake.lock
  commit=$(git -C "$tree" rev-parse HEAD)
  lease_take "$LOCK" -s 0 || fail "cannot take $LOCK"
  stable=$(nix build --no-link --print-out-paths "git+file://$tree?rev=$commit#packages.$SYSTEM.tomoe-stable") || stable=""
  lease_release
  if [[ -z $stable ]]; then
    $dry || { git -C "$tree" reset -q --soft HEAD^ && git -C "$tree" restore -q --staged --worktree -- flake.nix flake.lock; }
    fail "tomoe-stable does not build at finix $commit"
  fi
  echo "release: tomoe-stable at finix $commit is $stable:"
  cat "$stable/bin/tomoe-stable"
  if $dry; then
    git -C "$tree" show --stat --format='%h %s' HEAD | head -5
    git -C "$tree" show HEAD -- flake.nix
    git -C "$FINIX" worktree remove --force "$tree"
    rmdir "${tree%/finix}"
    return 0
  fi
  remote=github
  git -C "$tree" remote | grep -qx "$remote" || remote=origin
  git -C "$tree" push -q "$remote" "$commit:refs/heads/main"
  echo "release: pushed finix $commit; finix-switch $commit installs it"
}

promote_candidate() {
  parse_candidate "$1"
  last=$(candidates "$version" | tail -1)
  [[ $last == "$number" ]] || fail "v$version-rc.$last is newer than $1; promote that one"
  released "$version" && fail "v$version is already released"
  records=$(git notes --ref "$NOTES" show "$rev" 2>/dev/null | grep '^soak ' || true)
  [[ -n $records ]] || fail "$1 has never been soaked: nix run .#release -- soak $1"
  if grep -q '^soak FAIL' <<<"$records"; then
    fail "a soak of $rev failed; cut the next candidate:
$(grep '^soak FAIL' <<<"$records")"
  fi
  pass=$(grep "^soak PASS tag=$1 " <<<"$records" | tail -1)
  [[ -n $pass ]] || fail "no passing soak recorded for $1"
  seconds=$(sed -n 's/.* seconds=\([0-9]*\) .*/\1/p' <<<"$pass")
  finix=$(sed -n 's/.* finix=\([0-9a-f]*\) .*/\1/p' <<<"$pass")
  ((seconds >= SOAK)) || fail "$1 soaked ${seconds}s, less than the ${SOAK}s a release needs"
  notes=$(changelog "$version" "$rev")
  tag=v$version
  if $dry; then
    git update-ref "$TAGS/$tag" "$rev"
  else
    git tag -a "$tag" "$rev" -m "Tomoe $version"
    git push -q origin "refs/tags/$tag"
    printf '%s\n' "$notes" | gh release create "$tag" --verify-tag --title "Tomoe $version" --notes-file -
  fi
  echo "release: $tag is $rev, soaked ${seconds}s with finix $finix"
  move_stable "$tag" "$finix" "$rev"
}

case $command:$# in
rc:2 | rc:3) cut_candidate "$2" "${3:-}" ;;
soak:2) soak_candidate "$2" ;;
promote:2) promote_candidate "$2" ;;
*) usage ;;
esac
