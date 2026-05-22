#!/usr/bin/env bash


# Discover and run whatever integration-test executables have already been
# built under build/ITs/.
#
# This script does NOT build anything. Its only job is:
#   - discover existing test executables;
#   - run them;
#   - store their logs;
#   - produce a summary;
#   - generate coverage only if coverage data actually exists.


# -e: stop immediately if a command fails.
# -u: stop if an unset variable is used.
# -o pipefail: fail a pipeline if any command in the pipeline fails.
set -euo pipefail

html_escape_text() {
    sed \
        -e 's/&/\&amp;/g' \
        -e 's/</\&lt;/g' \
        -e 's/>/\&gt;/g'
}

html_escape_string() {
    printf '%s' "$1" | sed \
        -e 's/&/\&amp;/g' \
        -e 's/</\&lt;/g' \
        -e 's/>/\&gt;/g' \
        -e 's/"/\&quot;/g' \
        -e "s/'/\&#39;/g"
}

write_log_html() {
    local text_log="$1"
    local html_log="$2"
    local title="$3"
    local escaped_title

    escaped_title="$(html_escape_string "${title}")"

    {
        cat <<EOF
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escaped_title}</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f4f1ea;
      --panel: #fffdf8;
      --ink: #1f1a17;
      --muted: #6f6258;
      --line: #d8cabd;
      --accent: #9a3412;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "Iosevka Web", "IBM Plex Mono", "SFMono-Regular", Consolas, monospace;
      background: linear-gradient(180deg, #efe7dc 0%, var(--bg) 100%);
      color: var(--ink);
    }
    main {
      max-width: 1100px;
      margin: 0 auto;
      padding: 32px 20px 48px;
    }
    a { color: var(--accent); }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 20px;
      box-shadow: 0 16px 40px rgba(76, 55, 40, 0.08);
    }
    h1 {
      margin: 0 0 8px;
      font-size: 1.4rem;
    }
    p {
      margin: 0 0 18px;
      color: var(--muted);
    }
    pre {
      margin: 0;
      overflow-x: auto;
      white-space: pre-wrap;
      word-break: break-word;
      line-height: 1.45;
    }
  </style>
</head>
<body>
  <main>
    <div class="panel">
      <h1>${escaped_title}</h1>
      <p><a href="../index.html">Back to run summary</a></p>
      <pre>
EOF
        html_escape_text < "${text_log}"
        cat <<'EOF'
      </pre>
    </div>
  </main>
</body>
</html>
EOF
    } > "${html_log}"
}

write_run_index_html() {
    local index_file="$1"
    local escaped_run_id
    local escaped_generated_at
    local coverage_note_html=''
    local coverage_links_html=''
    local build_logs_html=''

    escaped_run_id="$(html_escape_string "${RUN_ID}")"
    escaped_generated_at="$(html_escape_string "${GENERATED_AT}")"

    if [[ -f "${RUN_RESULT_DIR}/build_libs.log" ]]; then
        build_logs_html+='<li><a href="build_libs.log">build_libs.log</a></li>'
    fi
    if [[ -f "${RUN_RESULT_DIR}/build_ITs.log" ]]; then
        build_logs_html+='<li><a href="build_ITs.log">build_ITs.log</a></li>'
    fi

    if [[ -n "${COVERAGE_STATUS_NOTE}" ]]; then
        coverage_note_html="$(html_escape_string "${COVERAGE_STATUS_NOTE}")"
    fi

    if [[ -f "${RUN_RESULT_DIR}/ITs_all_coverage.html" ]]; then
        coverage_links_html+='<li><a href="ITs_all_coverage.html">coverage HTML</a></li>'
    fi
    if [[ -f "${RUN_RESULT_DIR}/ITs_all_coverage.xml" ]]; then
        coverage_links_html+='<li><a href="ITs_all_coverage.xml">coverage XML</a></li>'
    fi
    if [[ -f "${RUN_RESULT_DIR}/coverage-summary.json" ]]; then
        coverage_links_html+='<li><a href="coverage-summary.json">coverage JSON summary</a></li>'
    fi

    {
        cat <<EOF
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>fsutil integration run ${escaped_run_id}</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #efe7dc;
      --panel: #fffdf8;
      --ink: #201814;
      --muted: #6a5a50;
      --line: #d8cabd;
      --accent: #b45309;
      --accent-2: #0f766e;
      --pass: #166534;
      --fail: #b91c1c;
      --shadow: rgba(76, 55, 40, 0.10);
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--ink);
      font-family: "IBM Plex Sans", "Segoe UI", sans-serif;
      background:
        radial-gradient(circle at top left, rgba(180, 83, 9, 0.12), transparent 28%),
        radial-gradient(circle at top right, rgba(15, 118, 110, 0.10), transparent 24%),
        var(--bg);
    }
    main {
      max-width: 1180px;
      margin: 0 auto;
      padding: 36px 20px 56px;
    }
    .hero, .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 20px;
      box-shadow: 0 18px 45px var(--shadow);
    }
    .hero {
      padding: 28px;
      margin-bottom: 22px;
    }
    .hero h1 {
      margin: 0 0 8px;
      font-size: 2rem;
    }
    .hero p {
      margin: 0;
      color: var(--muted);
      max-width: 60rem;
      line-height: 1.5;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 18px;
      margin-bottom: 22px;
    }
    .panel {
      padding: 22px;
    }
    h2 {
      margin: 0 0 14px;
      font-size: 1.05rem;
    }
    ul {
      margin: 0;
      padding-left: 18px;
    }
    li + li {
      margin-top: 8px;
    }
    a {
      color: var(--accent);
      text-decoration-thickness: 2px;
      text-underline-offset: 0.18em;
    }
    code {
      font-family: "Iosevka Web", "IBM Plex Mono", "SFMono-Regular", Consolas, monospace;
      font-size: 0.95em;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      overflow: hidden;
      border-radius: 16px;
      background: var(--panel);
      border: 1px solid var(--line);
    }
    th, td {
      padding: 14px 16px;
      border-bottom: 1px solid var(--line);
      text-align: left;
      vertical-align: top;
    }
    th {
      font-size: 0.82rem;
      letter-spacing: 0.06em;
      text-transform: uppercase;
      color: var(--muted);
      background: rgba(216, 202, 189, 0.26);
    }
    tr.pass td:first-child {
      border-left: 5px solid rgba(22, 101, 52, 0.85);
    }
    tr.fail td:first-child {
      border-left: 5px solid rgba(185, 28, 28, 0.85);
    }
    .status-pass { color: var(--pass); font-weight: 700; }
    .status-fail { color: var(--fail); font-weight: 700; }
    .muted { color: var(--muted); }
  </style>
</head>
<body>
  <main>
    <section class="hero">
      <h1>fsutil integration pipeline run</h1>
      <p>
        Run <code>${escaped_run_id}</code> archived the discovered integration-test executables,
        their logs, and any coverage reports produced from the built library set.
      </p>
    </section>

    <section class="grid">
      <div class="panel">
        <h2>Run Metadata</h2>
        <ul>
          <li>Run id: <code>${escaped_run_id}</code></li>
          <li>Generated at: <code>${escaped_generated_at}</code></li>
          <li>Text summary: <a href="integration_result.txt">integration_result.txt</a></li>
          <li>Project overview: <a href="../../index.html">results index</a></li>
        </ul>
      </div>
      <div class="panel">
        <h2>Build Logs</h2>
        <ul>
EOF
        if [[ -n "${build_logs_html}" ]]; then
            printf '%s\n' "${build_logs_html}"
        else
            printf '<li class="muted">No pipeline build logs were archived for this run.</li>\n'
        fi
        cat <<EOF
        </ul>
      </div>
      <div class="panel">
        <h2>Coverage</h2>
        <ul>
EOF
        if [[ -n "${coverage_links_html}" ]]; then
            printf '%s\n' "${coverage_links_html}"
        else
            printf '<li class="muted">%s</li>\n' "${coverage_note_html:-Coverage was not produced for this run.}"
        fi
        cat <<'EOF'
        </ul>
      </div>
    </section>

    <section class="panel">
      <h2>Test Matrix</h2>
      <table>
        <thead>
          <tr>
            <th>Profile</th>
            <th>Variant</th>
            <th>Status</th>
            <th>Exit Code</th>
            <th>Logs</th>
          </tr>
        </thead>
        <tbody>
EOF
        local i
        local row_class
        local status_label
        local status_class
        local escaped_profile
        local escaped_variant

        for i in "${!RESULT_PROFILES[@]}"; do
            if (( RESULT_EXIT_CODES[i] == 0 )); then
                row_class='pass'
                status_label='PASS'
                status_class='status-pass'
            else
                row_class='fail'
                status_label='FAIL'
                status_class='status-fail'
            fi

            escaped_profile="$(html_escape_string "${RESULT_PROFILES[i]}")"
            escaped_variant="$(html_escape_string "${RESULT_VARIANTS[i]}")"

            printf '          <tr class="%s">\n' "${row_class}"
            printf '            <td><code>%s</code></td>\n' "${escaped_profile}"
            printf '            <td><code>%s</code></td>\n' "${escaped_variant}"
            printf '            <td class="%s">%s</td>\n' "${status_class}" "${status_label}"
            printf '            <td><code>%d</code></td>\n' "${RESULT_EXIT_CODES[i]}"
            printf '            <td><a href="%s">html</a> <span class="muted">|</span> <a href="%s">txt</a></td>\n' \
                "${RESULT_HTML_LOGS[i]}" "${RESULT_TEXT_LOGS[i]}"
            printf '          </tr>\n'
        done

        cat <<'EOF'
        </tbody>
      </table>
    </section>
  </main>
</body>
</html>
EOF
    } > "${index_file}"
}

write_results_overview_html() {
    local overview_file="$1"
    local escaped_generated_at
    local latest_run_rel="runs/${RUN_ID}/index.html"
    local latest_run_rel_escaped
    local i

    escaped_generated_at="$(html_escape_string "${GENERATED_AT}")"
    latest_run_rel_escaped="$(html_escape_string "${latest_run_rel}")"

    mapfile -t run_index_files < <(
        find "${RUNS_DIR}" -mindepth 2 -maxdepth 2 -type f -name 'index.html' | sort -r
    )

    {
        cat <<EOF
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>fsutil integration results</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f5efe4;
      --panel: #fffdf8;
      --ink: #221b16;
      --muted: #6f6154;
      --line: #dfd1c2;
      --accent: #0f766e;
      --accent-2: #b45309;
      --shadow: rgba(76, 55, 40, 0.08);
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "IBM Plex Sans", "Segoe UI", sans-serif;
      color: var(--ink);
      background:
        linear-gradient(180deg, rgba(15, 118, 110, 0.09), transparent 20%),
        linear-gradient(180deg, rgba(180, 83, 9, 0.06), transparent 38%),
        var(--bg);
    }
    main {
      max-width: 980px;
      margin: 0 auto;
      padding: 34px 20px 52px;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 20px;
      box-shadow: 0 16px 40px var(--shadow);
      padding: 24px;
    }
    .panel + .panel {
      margin-top: 20px;
    }
    h1, h2 {
      margin: 0 0 10px;
    }
    p {
      margin: 0;
      color: var(--muted);
      line-height: 1.5;
    }
    ul {
      margin: 14px 0 0;
      padding-left: 20px;
    }
    li + li {
      margin-top: 10px;
    }
    a {
      color: var(--accent);
      text-decoration-thickness: 2px;
      text-underline-offset: 0.18em;
    }
    code {
      font-family: "Iosevka Web", "IBM Plex Mono", "SFMono-Regular", Consolas, monospace;
      font-size: 0.95em;
    }
    .hero-link {
      display: inline-block;
      margin-top: 16px;
      padding: 10px 14px;
      border-radius: 999px;
      background: rgba(180, 83, 9, 0.10);
      color: var(--accent-2);
      text-decoration: none;
      font-weight: 700;
    }
  </style>
</head>
<body>
  <main>
    <section class="panel">
      <h1>fsutil integration results</h1>
      <p>
        Every archived integration run lives under <code>tests/results/ITs/runs/</code>.
        Open the latest run in the browser, then drill into raw logs or coverage from there.
      </p>
      <a class="hero-link" href="${latest_run_rel_escaped}">Open latest run</a>
    </section>

    <section class="panel">
      <h2>Snapshot</h2>
      <p>Last refreshed at <code>${escaped_generated_at}</code>.</p>
      <ul>
        <li>Latest run directory: <code>tests/results/ITs/runs/${RUN_ID}</code></li>
        <li>Latest run summary: <a href="integration_result.txt">integration_result.txt</a></li>
      </ul>
    </section>

    <section class="panel">
      <h2>Run History</h2>
      <ul>
EOF
        if ((${#run_index_files[@]} == 0)); then
            printf '<li>No runs have been archived yet.</li>\n'
        else
            for i in "${!run_index_files[@]}"; do
                run_rel_path="${run_index_files[i]#${RESULT_ROOT_DIR}/}"
                run_dir_name="$(basename "$(dirname "${run_index_files[i]}")")"
                printf '<li><a href="%s">%s</a></li>\n' \
                    "$(html_escape_string "${run_rel_path}")" \
                    "$(html_escape_string "${run_dir_name}")"
            done
        fi
        cat <<'EOF'
      </ul>
    </section>
  </main>
</body>
</html>
EOF
    } > "${overview_file}"
}

# Remember where the user launched the script.
START_DIR="$(pwd -P)"
# Return to the user's launch directory.
cleanup() { cd -- "$START_DIR"; }
# Always return to START_DIR on script exit.
trap cleanup EXIT

# Determine the directory that contains this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Determine the root directory of the project (the parent of the script's
# directory).
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

TEST_BUILD_DIR="${ROOT_DIR}/build/ITs"
RESULT_ROOT_DIR="${ROOT_DIR}/tests/results/ITs"
RUNS_DIR="${RESULT_ROOT_DIR}/runs"
RUN_ID="${FSUTIL_RESULTS_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RUN_RESULT_DIR="${RUNS_DIR}/${RUN_ID}"
SUMMARY_FILE="${RUN_RESULT_DIR}/integration_result.txt"
RUN_INDEX_FILE="${RUN_RESULT_DIR}/index.html"
OVERVIEW_FILE="${RESULT_ROOT_DIR}/index.html"
GENERATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

declare -a RESULT_PROFILES=()
declare -a RESULT_VARIANTS=()
declare -a RESULT_EXIT_CODES=()
declare -a RESULT_TEXT_LOGS=()
declare -a RESULT_HTML_LOGS=()

COVERAGE_STATUS_NOTE='coverage was not produced for this run'

mkdir -p "${RUN_RESULT_DIR}"
: > "${SUMMARY_FILE}"

printf 'running integration tests discovered under %s\n' "${TEST_BUILD_DIR}"
printf 'archiving results under %s\n' "${RUN_RESULT_DIR}"

if [[ ! -d "${TEST_BUILD_DIR}" ]]; then
    printf 'test build directory not found: %s\n' "${TEST_BUILD_DIR}" >&2
    printf 'run ./utils/build_ITs.sh first\n' >&2
    exit 1
fi

# Discover test artifacts by their canonical names.
#
# We do not rely on the executable bit here because GitHub Actions artifact
# upload/download normalizes file permissions across jobs. The known test
# binaries therefore need to be rediscovered by name and re-marked executable
# before they are launched.
mapfile -t test_executables < <(
    find "${TEST_BUILD_DIR}" -mindepth 2 -maxdepth 2 -type f \
        \( -name 'integration_test.shared' -o -name 'integration_test.static' \) \
        | sort
)

if ((${#test_executables[@]} == 0)); then
    printf 'no built integration-test executables were found in %s\n' "${TEST_BUILD_DIR}" >&2
    printf 'run ./utils/build_ITs.sh first\n' >&2
    exit 1
fi

overall_rc=0

for test_executable in "${test_executables[@]}"; do
    relative_path="${test_executable#${TEST_BUILD_DIR}/}"
    profile="${relative_path%%/*}"
    executable_name="${test_executable##*/}"
    variant="${executable_name#integration_test.}"

    profile_result_dir="${RUN_RESULT_DIR}/${profile}"
    log_file="${profile_result_dir}/${executable_name}.txt"
    html_log_file="${profile_result_dir}/${executable_name}.html"
    mkdir -p "${profile_result_dir}"

    printf '\n[%s / %s]\n' "${profile}" "${variant}"
    printf '  running:  %s\n' "${test_executable}"
    printf '  log file: %s\n' "${log_file}"

    chmod u+x "${test_executable}"

    run_rc=0
    {
        printf '[%s / %s]\n' "${profile}" "${variant}"
        "${test_executable}"
    } 2>&1 | tee "${log_file}" || run_rc=$?

    {
        printf '[%s / %s]\n' "${profile}" "${variant}"
        printf 'log: %s\n' "${log_file}"
        printf 'exit code: %d\n' "${run_rc}"
        printf '\n'
        cat "${log_file}"
        printf '\n\n'
    } >> "${SUMMARY_FILE}"

    if ((run_rc == 0)); then
        printf '  result: PASS\n'
    else
        printf '  result: FAIL (exit code %d)\n' "${run_rc}"
        overall_rc="${run_rc}"
    fi

    write_log_html "${log_file}" "${html_log_file}" "fsutil integration log: ${profile} / ${variant}"

    RESULT_PROFILES+=("${profile}")
    RESULT_VARIANTS+=("${variant}")
    RESULT_EXIT_CODES+=("${run_rc}")
    RESULT_TEXT_LOGS+=("${profile}/${executable_name}.txt")
    RESULT_HTML_LOGS+=("${profile}/${executable_name}.html")
done

# Coverage is optional.
#
# The old all-in-one script always rebuilt fsutil.c with --coverage, so reports
# were guaranteed. The new workflow instead links tests against prebuilt
# libraries. Coverage reports therefore exist only when a coverage-instrumented
# library variant such as release_cov was built and exercised.
coverage_artifact_count="$(
    find "${ROOT_DIR}/build" -type f \( -name '*.gcno' -o -name '*.gcda' \) \
        ! -path "${ROOT_DIR}/build/ITs/*" \
        | wc -l
)"

if (( coverage_artifact_count > 0 )); then
    if ! command -v gcovr >/dev/null 2>&1; then
        printf '\n[coverage] coverage artifacts were found, but gcovr is not installed\n' >&2
    else
        printf '\n[coverage] generating reports via gcovr...\n'

        gcovr -r "${ROOT_DIR}" \
            --object-directory "${ROOT_DIR}/build" \
            --exclude 'tests/' \
            --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
            --html --html-details \
            -o "${RUN_RESULT_DIR}/ITs_all_coverage.html"

        gcovr -r "${ROOT_DIR}" \
            --object-directory "${ROOT_DIR}/build" \
            --exclude 'tests/' \
            --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
            --xml \
            -o "${RUN_RESULT_DIR}/ITs_all_coverage.xml"

        gcovr -r "${ROOT_DIR}" \
            --object-directory "${ROOT_DIR}/build" \
            --exclude 'tests/' \
            --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
            --json-summary \
            -o "${RUN_RESULT_DIR}/coverage-summary.json"

        printf '[coverage] report ready: %s\n' "${RUN_RESULT_DIR}/ITs_all_coverage.html"
        COVERAGE_STATUS_NOTE='coverage reports generated'
    fi
else
    printf '\n[coverage] skipped: no coverage-instrumented build artifacts were found under %s\n' "${ROOT_DIR}/build"
    printf '[coverage] build a coverage-enabled library variant first if you want reports\n'
    COVERAGE_STATUS_NOTE='no coverage-instrumented library artifacts were found under build/'
fi

printf '\nsummary file: %s\n' "${SUMMARY_FILE}"

write_run_index_html "${RUN_INDEX_FILE}"
ln -sfn "runs/${RUN_ID}" "${RESULT_ROOT_DIR}/latest"
cp "${SUMMARY_FILE}" "${RESULT_ROOT_DIR}/integration_result.txt"
write_results_overview_html "${OVERVIEW_FILE}"

printf 'browser index: %s\n' "${RUN_INDEX_FILE}"
printf 'browser overview: %s\n' "${OVERVIEW_FILE}"

exit "${overall_rc}"
