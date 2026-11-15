# Releasing

Releases are built and published by `.github/workflows/release.yml` from
annotated stable tags. Do not create a GitHub release or tag through the web
interface: the workflow requires an annotated tag and validates the tag,
version, changelog, and exact-commit CI result before publishing.

## Repository settings

Immutable releases are enabled for this repository. This is a one-time
administrative setting, not a per-release step. Confirm it before the first
release on a new repository:

```sh
gh api repos/mmabdpr/mpmc-queue-cpp/immutable-releases --jq .enabled
```

The release workflow cannot enable or verify this setting itself.

## CI toolchain

CI uses the exact versions and SHA-256 downloads in
[`ci/toolchain.lock`](ci/toolchain.lock), full commit SHAs for GitHub Actions,
and explicit runner/compiler names. The workflows do not use `*-latest`, apt
installs, or floating tool versions. `ci/verify-toolchain.sh` fails the job
when the image no longer matches.

Refresh the lock deliberately, run the full CI workflow, and review the diff.
Pinned tools do not receive automatic security updates. GitHub-hosted runner
images, kernels, hardware, and macOS/Xcode availability remain external; the
immutable release and checksums are the durable record.

## Prepare

1. Update the version in `CMakeLists.txt` and all four version definitions in
   `include/mpmc/version.hpp`.
2. Add a matching `## X.Y.Z - YYYY-MM-DD` section to `CHANGELOG.md`.
3. Merge the release commit to `main` and wait for the `CI` workflow to pass.
4. Confirm the working tree is clean and `main` matches `origin/main`.

## Publish

```sh
version=X.Y.Z  # Replace with the new, unpublished version.
git switch main
git pull --ff-only
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"
git tag -a "v$version" -m "mpmc_queue $version"
git push origin "v$version"
```

The tag push starts the release workflow. It verifies that the tag is
annotated, points to a commit on `main`, matches the CMake and header versions,
has a nonempty changelog section, and has a successful `CI` run for the tagged
commit. It rebuilds and tests the source archive, checks the headers archive,
creates a draft release, verifies its assets, and then publishes it with:

- `mpmc_queue-X.Y.Z.tar.gz`: complete source tree;
- `mpmc_queue-X.Y.Z-headers.tar.gz`: headers and documentation;
- `SHA256SUMS`: checksums for both archives.

Published immutable tags and assets cannot be moved, replaced, or deleted
while the release exists. The release title and notes remain editable. Deleting
an immutable release removes its assets and permanently prevents reuse of its
tag name, so publish a new patch version instead of retagging a published
release.

## Verify

```sh
gh release view "v$version" -R mmabdpr/mpmc-queue-cpp
gh release verify "v$version" -R mmabdpr/mpmc-queue-cpp
```

## Recovery

For a transient failure before publication, use **Actions → Release → Run
workflow** on `main` with the existing tag. Manual dispatch uses the workflow
definition on `main`, but checks out the release tag. Changes to checked-out
files such as `tools/release.py` require a new commit and, for an unpublished
release only, recreating the tag from the corrected commit.

If verification fails before publication, fix the problem on `main`, wait for
the corrected commit's `CI` run to pass, and then delete and recreate the
unpublished tag:

```sh
version=X.Y.Z  # Only for a version that has not been published.
git switch main
git pull --ff-only
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"
git push origin ":refs/tags/v$version"
git tag -d "v$version"
git tag -a "v$version" -m "mpmc_queue $version"
git push origin "v$version"
```

Once a release is published, never move, replace, or reuse its tag. Publish a
new patch version instead.
