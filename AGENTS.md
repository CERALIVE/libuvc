# libuvc

Parent: https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md

<!-- workspace-hard-rules:begin -->
## Workspace hard rules (identical in every CeraLive AGENTS.md)
- Commits and PRs carry the human author only: no Co-authored-by, no AI attribution.
- Start from the updated canonical branch; rebase to update; never `reset --hard` or discard others' work.
- One focused PR per repo, opened against CERALIVE/<repo>; the root policy PR merges first.
- A repo is self-contained: no path above its root; consume @ceralive packages from the registry, never link:/file:.
- Never delete, skip or weaken a test; every behavior change ships with a test.
- A user-visible change updates docs.ceralive.tv in English and Spanish (es-419), and any ceralive.tv claim it touches, in the same release.
- AGENTS.md holds rules and routing only, within budget; contracts and history live in docs/agents/.
- Full canon: https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md
<!-- workspace-hard-rules:end -->

## ROLE

CeraLive's libusb-based UVC library fork, maintained for
[`gstlibuvcsrc`](https://github.com/CERALIVE/gstlibuvcsrc).
Adds UVC 1.5 header acceptance, H.265 support and Linux driver-reattach protection.
Hard divergence from libuvc v0.0.7; the upstream base is provenance, not a sync target.

## STRUCTURE

- `src/` — library implementation and driver-reattach helper.
- `include/` — public headers and generated configuration template.
- `cmake/` — dependency discovery modules.
- `tests/` — hardware-independent regression suite.
- `docs/` — engineering evidence.
- `cameras/` — camera data.
- `.github/workflows/` — build, regression and ThreadSanitizer gates.

## COMMANDS

Prerequisites: CMake, a C compiler, libusb, libjpeg and jq (CI dependency list).
Local builds do not require ccache. Run both shared-library auto-detach variants:

```bash
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF -DBUILD_TESTING=OFF -DLIBUVC_AUTO_DETACH_KERNEL_DRIVER=ON
cmake --build build --parallel
cmake -B build-off -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF -DBUILD_TESTING=OFF -DLIBUVC_AUTO_DETACH_KERNEL_DRIVER=OFF
cmake --build build-off --parallel
test -f build/libuvc.so && test -f build/libuvc.a
```

Hardware-independent suite and guard-off rollback:

```bash
cmake -S . -B build/regression -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_BUILD_TARGET=Static -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF -DBUILD_TESTING=ON
cmake --build build/regression --parallel
ctest --test-dir build/regression --show-only=json-v1 | jq -e '.tests | length == 37'
ctest --test-dir build/regression --output-on-failure
cmake -S . -B build-noguard -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_BUILD_TARGET=Static -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF -DBUILD_TESTING=ON -DLIBUVC_REATTACH_GUARD=OFF
cmake --build build-noguard --parallel
ctest --test-dir build-noguard --show-only=json-v1 | jq -e '(.tests | length == 27) and ([.tests[].name] | map(startswith("libuvc.reattach.")) | any | not)'
ctest --test-dir build-noguard --output-on-failure
```

ThreadSanitizer: follow [README sanitized builds](README.md#sanitized-builds)
and the exact commands in `.github/workflows/build.yml`; keep its scoped test filter.
`BUILD_TEST` is an interactive camera/display demo, not the automated suite.

## WHERE TO LOOK

| Task | Contract |
|------|----------|
| Fork scope, base SHA and options | [README.md](README.md) |
| USB teardown, quarantine and reattach | [README device teardown contract](README.md#device-teardown-contract) |
| Regression scope | [Compatibility evidence](docs/evidence/uvc-camera-compat-stability.md) |
| Changes from the fork base | [CHANGELOG.ceralive.md](CHANGELOG.ceralive.md) |
| Shared-library version and build options | `CMakeLists.txt` |
| Exact test inventory and sanitizer gate | `.github/workflows/build.yml` |

## HARD RULES

- Keep this BSD-3-Clause fork maintained for gstlibuvcsrc; CeraLive additions keep the same license.
- Source dependency only: no `.deb`, and not in device-image `REPOS`.
- Keep the libuvc SONAMEs; `CMakeLists.txt` derives shared-library SOVERSION from the existing major version.
- Do not pull from or rebase onto upstream libuvc main; the base SHA records provenance only.
- Preserve the README's five USB teardown invariants and armed quarantine behavior.
