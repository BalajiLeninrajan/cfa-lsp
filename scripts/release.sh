#!/usr/bin/env bash
# Usage: scripts/release.sh VERSION   (or make release V=VERSION)
#
# Tags origin/main as vVERSION and pushes the tag. The tag's CI run builds and
# tests it like any other, then publishes a GitHub release with the binaries
# (.github/workflows/ci.yml). Nothing is built here.
#
# Refuses unless the checkout is main, clean and the same as origin/main, CI
# passed on that commit, and VERSION is new and higher than the last release.
# A VERSION with a suffix (0.3.0-rc1) is published as a prerelease.
set -euo pipefail

die() { echo "release: $*" >&2; exit 1; }

version=${1:-}
version=${version#v}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] \
	|| die "usage: release.sh VERSION, e.g. 0.2.0 or 0.3.0-rc1"
tag=v$version

cd "$(git rev-parse --show-toplevel)"
command -v gh > /dev/null || die "needs the GitHub CLI (gh)"

[ "$(git branch --show-current)" = main ] || die "not on main"
git diff --quiet HEAD -- || die "uncommitted changes (submodule included)"
git fetch -q --tags origin main
head=$(git rev-parse HEAD)
[ "$head" = "$(git rev-parse origin/main)" ] || die "main isn't origin/main; pull or push first"

git rev-parse -q --verify "refs/tags/$tag" > /dev/null && die "$tag already exists"
last=$(git tag -l 'v[0-9]*' --sort=-v:refname | head -n 1)
if [ -n "$last" ] && [ "$(printf '%s\n%s\n' "${last#v}" "$version" | sort -V | tail -n 1)" != "$version" ]; then
	die "$tag isn't higher than the last release, $last"
fi

run=$(gh run list --workflow CI --branch main --commit "$head" --event push --limit 1 \
	--json status,conclusion,url --jq '.[0] | "\(.status) \(.conclusion) \(.url)"')
[ -n "$run" ] || die "no CI run for $head yet"
read -r status conclusion url <<< "$run"
[ "$status $conclusion" = "completed success" ] || die "CI on $head is $status${conclusion:+/$conclusion}: $url"

git tag -a "$tag" -m "cfa-lsp $version"
git push -q origin "$tag"
echo "Pushed $tag ($(git log -1 --format=%s "$head"))."
echo "CI publishes the release when the tag's run passes:"
echo "  gh run watch \$(gh run list --branch $tag --limit 1 --json databaseId --jq '.[0].databaseId')"
