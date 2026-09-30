#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  echo "usage: $0 {dependencies|static|fuzz|artifacts|all} --build-dir PATH --source-commit SHA [--repo-root PATH]" >&2
  exit 2
}

[[ $# -ge 1 ]] || usage
MODE="$1"
shift
case "$MODE" in
  dependencies|static|fuzz|artifacts|all) ;;
  *) usage ;;
esac

REPO_ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR=""
SOURCE_COMMIT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo-root)
      [[ $# -ge 2 ]] || usage
      REPO_ROOT="$2"
      shift 2
      ;;
    --build-dir)
      [[ $# -ge 2 ]] || usage
      BUILD_DIR="$2"
      shift 2
      ;;
    --source-commit)
      [[ $# -ge 2 ]] || usage
      SOURCE_COMMIT="$2"
      shift 2
      ;;
    *) usage ;;
  esac
done

[[ -n "$BUILD_DIR" && -n "$SOURCE_COMMIT" ]] || usage
[[ "$SOURCE_COMMIT" =~ ^[0-9a-fA-F]{40}$ ]] || {
  echo "source commit must be a complete 40-character Git SHA" >&2
  exit 2
}
REPO_ROOT="$(realpath "$REPO_ROOT")"
BUILD_DIR="$(realpath -m "$BUILD_DIR")"
[[ -d "$REPO_ROOT/.git" || -f "$REPO_ROOT/.git" ]] || {
  echo "repository root is not a Git worktree: $REPO_ROOT" >&2
  exit 2
}
case "$BUILD_DIR" in
  /|"$REPO_ROOT")
    echo "refusing broad or source-tree build directory: $BUILD_DIR" >&2
    exit 2
    ;;
esac
mkdir -p "$BUILD_DIR"

DEPENDENCY_POLICY="$REPO_ROOT/tools/sbom_expected.json"
ASSURANCE_POLICY="$REPO_ROOT/release/portable-assurance-policy.json"

run_dependencies() {
  local output="$BUILD_DIR/dependency-report.json"
  [[ ! -e "$output" ]] || {
    echo "dependency output already exists: $output" >&2
    return 2
  }
  python3 "$REPO_ROOT/tools/sbom.py" verify-dependencies \
    --repo-root "$REPO_ROOT" \
    --policy "$DEPENDENCY_POLICY" \
    --output "$output"
}

run_static() {
  command -v clang-14 >/dev/null
  command -v clang++-14 >/dev/null
  command -v clang-tidy-14 >/dev/null
  command -v cmake >/dev/null
  local static_build="$BUILD_DIR/static"
  CC=clang-14 CXX=clang++-14 cmake \
    -S "$REPO_ROOT" \
    -B "$static_build" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_TESTS=OFF \
    -DRTFW_BUILD_EXPERIMENTAL=OFF \
    -DRTFW_BUILD_EXAMPLES=OFF \
    -DRTFW_BUILD_RUNTIME_DEMO=OFF \
    -DSIM_DOWNLOAD_GTEST=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DSIM_WERROR=ON
  python3 "$REPO_ROOT/tools/check_static_analysis.py" \
    --repo-root "$REPO_ROOT" \
    --compile-commands "$static_build/compile_commands.json" \
    --source-manifest "$REPO_ROOT/tools/static_analysis_sources.txt" \
    --clang-tidy clang-tidy-14 \
    --output "$BUILD_DIR/static-report.json"
}

run_fuzz() {
  command -v clang-14 >/dev/null
  command -v clang++-14 >/dev/null
  command -v cmake >/dev/null
  local fuzz_build="$BUILD_DIR/fuzz"
  CC=clang-14 CXX=clang++-14 cmake \
    -S "$REPO_ROOT" \
    -B "$fuzz_build" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_TESTS=ON \
    -DRTFW_BUILD_EXPERIMENTAL=ON \
    -DENABLE_RAPIDCHECK=OFF \
    -DSIM_BUILD_FUZZERS=ON \
    -DSIM_DOWNLOAD_GTEST=OFF \
    -DSIM_SANITIZERS= \
    -DSIM_WERROR=ON
  cmake --build "$fuzz_build" \
    --target snapshot_fuzz runtime_profile_fuzz jobqueue_fuzz \
    --parallel 2
  python3 "$REPO_ROOT/tools/run_fuzz_smoke.py" \
    --build-dir "$fuzz_build" \
    --corpus-root "$REPO_ROOT/tests/fuzz" \
    --output-dir "$BUILD_DIR/fuzz-artifacts" \
    --seed 424242
}

source_tree_state() {
  if [[ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=all)" ]]; then
    echo dirty
  else
    echo clean
  fi
}

run_artifacts() {
  command -v clang-14 >/dev/null
  command -v clang++-14 >/dev/null
  command -v cmake >/dev/null
  command -v cpack >/dev/null
  local package_build="$BUILD_DIR/package"
  local cpack_output="$BUILD_DIR/cpack"
  local candidate_a="$BUILD_DIR/candidate-a"
  local candidate_b="$BUILD_DIR/candidate-b"
  local relocated="$BUILD_DIR/relocated"
  local consumer="$BUILD_DIR/consumer"
  for path in "$cpack_output" "$candidate_a" "$candidate_b" "$relocated" "$consumer"; do
    [[ ! -e "$path" ]] || {
      echo "artifact-mode output already exists: $path" >&2
      return 2
    }
  done

  CC=clang-14 CXX=clang++-14 cmake \
    -S "$REPO_ROOT" \
    -B "$package_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTS=OFF \
    -DRTFW_BUILD_EXPERIMENTAL=OFF \
    -DRTFW_BUILD_EXAMPLES=OFF \
    -DRTFW_BUILD_RUNTIME_DEMO=OFF \
    -DSIM_WERROR=ON
  cmake --build "$package_build" --parallel 2
  cpack --config "$package_build/CPackConfig.cmake" \
    -C Release -G TGZ -B "$cpack_output"
  python3 "$REPO_ROOT/tools/stage_release_artifacts.py" \
    --cpack-dir "$cpack_output" \
    --artifact-dir "$candidate_a" \
    --generator TGZ \
    --version-file "$REPO_ROOT/VERSION.txt"
  mkdir "$candidate_b"
  find "$candidate_a" -maxdepth 1 -type f -exec cp {} "$candidate_b/" \;

  python3 "$REPO_ROOT/tools/sbom.py" create \
    --repo-root "$REPO_ROOT" \
    --artifact-dir "$candidate_a" \
    --output "$candidate_a/rtfw.spdx.json" \
    --policy "$ASSURANCE_POLICY" \
    --dependency-policy "$DEPENDENCY_POLICY" \
    --source-commit "$SOURCE_COMMIT"
  python3 "$REPO_ROOT/tools/sbom.py" create \
    --repo-root "$REPO_ROOT" \
    --artifact-dir "$candidate_b" \
    --output "$candidate_b/rtfw.spdx.json" \
    --policy "$ASSURANCE_POLICY" \
    --dependency-policy "$DEPENDENCY_POLICY" \
    --source-commit "$SOURCE_COMMIT"
  cmp "$candidate_a/rtfw.spdx.json" "$candidate_b/rtfw.spdx.json"

  local tree_id
  tree_id="$(git -C "$REPO_ROOT" rev-parse HEAD^{tree})"
  local tree_state
  tree_state="$(source_tree_state)"
  local compiler_id
  compiler_id="$(clang++-14 --version | head -1)"
  local cmake_id
  cmake_id="$(cmake --version | head -1)"
  python3 "$REPO_ROOT/tools/provenance.py" create \
    --artifact-dir "$candidate_a" \
    --output "$candidate_a/rtfw.provenance.json" \
    --policy "$ASSURANCE_POLICY" \
    --dependency-policy "$DEPENDENCY_POLICY" \
    --source-commit "$SOURCE_COMMIT" \
    --source-tree "$tree_id" \
    --source-tree-state "$tree_state" \
    --compiler "$compiler_id" \
    --cmake "$cmake_id"
  python3 "$REPO_ROOT/tools/release_manifest.py" create \
    --artifact-dir "$candidate_a" \
    --output "$candidate_a/rtfw-release-manifest.json" \
    --version-file "$REPO_ROOT/VERSION.txt" \
    --source-commit "$SOURCE_COMMIT"
  python3 "$REPO_ROOT/tools/release_manifest.py" verify \
    --artifact-dir "$candidate_a" \
    --manifest "$candidate_a/rtfw-release-manifest.json" \
    --version-file "$REPO_ROOT/VERSION.txt" \
    --expected-source-commit "$SOURCE_COMMIT"
  python3 "$REPO_ROOT/tools/sbom.py" verify \
    --repo-root "$REPO_ROOT" \
    --artifact-dir "$candidate_a" \
    --sbom "$candidate_a/rtfw.spdx.json" \
    --policy "$ASSURANCE_POLICY" \
    --dependency-policy "$DEPENDENCY_POLICY" \
    --source-commit "$SOURCE_COMMIT"
  python3 "$REPO_ROOT/tools/provenance.py" verify \
    --artifact-dir "$candidate_a" \
    --statement "$candidate_a/rtfw.provenance.json" \
    --manifest "$candidate_a/rtfw-release-manifest.json" \
    --policy "$ASSURANCE_POLICY" \
    --dependency-policy "$DEPENDENCY_POLICY" \
    --source-commit "$SOURCE_COMMIT" \
    --source-tree "$tree_id" \
    --source-tree-state "$tree_state" \
    --compiler "$compiler_id" \
    --cmake "$cmake_id"
  python3 "$REPO_ROOT/tools/provenance.py" verify-fixture \
    --repo-root "$REPO_ROOT" \
    --policy "$ASSURANCE_POLICY"

  python3 "$REPO_ROOT/tools/extract_release_archive.py" \
    --artifact-dir "$candidate_a" \
    --destination "$relocated"
  CC=clang-14 CXX=clang++-14 cmake \
    -S "$REPO_ROOT/tests/package_consumer" \
    -B "$consumer" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$relocated"
  cmake --build "$consumer" --parallel 2
  ctest --test-dir "$consumer" --output-on-failure
  python3 "$REPO_ROOT/tools/check_c_abi.py" \
    --library "$package_build/librtfw.so"
  readelf -d "$package_build/librtfw.so" | \
    rg 'SONAME.*librtfw.so.8'

  python3 - "$candidate_a" "$BUILD_DIR/artifact-report.json" "$SOURCE_COMMIT" <<'PY'
import hashlib,json,pathlib,sys
root=pathlib.Path(sys.argv[1])
output=pathlib.Path(sys.argv[2])
records=[]
for path in sorted(root.iterdir()):
    if path.is_file() and not path.is_symlink():
        records.append({"name":path.name,"bytes":path.stat().st_size,"sha256":hashlib.sha256(path.read_bytes()).hexdigest()})
value={"schema_version":1,"source_commit":sys.argv[3],"candidate_only":True,"signed":False,"published":False,"authenticated_provenance":False,"files":records}
temporary=output.with_name(output.name+".tmp")
temporary.write_text(json.dumps(value,indent=2,sort_keys=True)+"\n",encoding="utf-8")
temporary.replace(output)
PY
}

case "$MODE" in
  dependencies) run_dependencies ;;
  static) run_static ;;
  fuzz) run_fuzz ;;
  artifacts) run_artifacts ;;
  all)
    run_dependencies
    run_static
    run_fuzz
    run_artifacts
    ;;
esac

echo "portable assurance mode=$MODE completed build_dir=$BUILD_DIR"
