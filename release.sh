fail() {
  echo "release: $*" >&2
  exit 1
}

version=${1:-}
publish=${2:-}
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$ || -n $publish && $publish != --publish ]]; then
  echo "usage: nix run .#release -- VERSION [--publish]" >&2
  echo "VERSION is X.Y.Z or X.Y.Z-rc.N. Without --publish, commit the version, check and build;" >&2
  echo "with it, also tag, push and create the GitHub release." >&2
  exit 2
fi
cd "$(git rev-parse --show-toplevel)" || exit
base=${version%%-rc.*}
tag=v$version

[[ $(git branch --show-current) == main ]] || fail "releases are cut from main"
[[ -z $(git status --porcelain) ]] || fail "the working tree has uncommitted changes"
! git rev-parse -q --verify "refs/tags/$tag" >/dev/null || fail "$tag already exists"
notes=$(awk -v heading="## $base" '$0 == heading { found = 1; next } /^## / && found { exit } found' CHANGELOG.md)
[[ -n ${notes//[[:space:]]/} ]] || fail "CHANGELOG.md has no '## $base' section"
mapfile -t declared < <(sed -n 's/^ *version = "\(.*\)";$/\1/p' flake.nix)
[[ ${#declared[@]} -eq 1 ]] || fail "flake.nix must declare exactly one version"

if [[ ${declared[0]} != "$version" ]]; then
  sed -i "s/^\( *version = \)\"${declared[0]}\";$/\1\"$version\";/" flake.nix
  git commit -q -m "Release $version" flake.nix
fi

nix flake check
out=$(nix build --no-link --print-out-paths)
[[ $("$out/bin/tomoe" --version) == "tomoe $version,"* ]] || fail "$out/bin/tomoe --version does not report $version"

if [[ -z $publish ]]; then
  echo "$out"
  exit 0
fi

git fetch -q origin main
[[ -z $(git rev-list HEAD..origin/main) ]] || fail "origin/main has commits that main lacks"
! git ls-remote --exit-code --tags origin "refs/tags/$tag" >/dev/null || fail "$tag already exists on origin"
repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner)
git tag -a "$tag" -m "Tomoe $version"
git push -q origin main "$tag"

flags=(--verify-tag --title "Tomoe $version" --notes-file -)
if [[ $version == *-rc.* ]]; then
  flags+=(--prerelease)
  notes=$(printf 'Release candidate %s for Tomoe %s. Try it with\n\n    nix run github:%s/%s\n\nand report problems at https://github.com/%s/issues.\n\n%s' \
    "${version##*-rc.}" "$base" "$repo" "$tag" "$repo" "$notes")
fi
printf '%s\n' "$notes" | gh release create "$tag" "${flags[@]}"
