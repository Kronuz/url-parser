#!/usr/bin/env python3
# Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
# SPDX-License-Identifier: MIT
"""Mutation check of the decoder, the scans and normalize_path.

Applies each of a set of small breaks to url_parser.cc in a scratch copy of the repository,
rebuilds the CTest suite, runs it, and requires that the suite fails (the mutant is killed) and
that the test the CHANGELOG credits with the kill is among the failures:

  D1-D14  the decoder, killed by `url_parser_spec`
  S1-S5   the vector scans of QueryParser::next and PathParser::next, killed by `url_parser_next_model`
  N1-N9   normalize_path and the plain-path test, killed by `url_parser_normalize`

Each mutant is an exact-anchor replacement that must match exactly once.

  python3 tools/mutants.py                run all, in a copy under the temp directory
  python3 tools/mutants.py --only D1,S3   run some (a letter such as D selects a group)
  python3 tools/mutants.py --list         print them
  python3 tools/mutants.py --check        verify every anchor occurs exactly once (no build; CTest
                                          test url_parser_mutants_anchors)

A run configures the copy once (it fetches char-classify like any build), checks that the
unmutated suite passes, and then spends one build and one ctest run per mutant. The working
tree is never touched. Exit status 0 means every mutant was killed by the test it names.
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = "url_parser.cc"
SPEC, NEXT, NORMALIZE = "url_parser_spec", "url_parser_next_model", "url_parser_normalize"


class Mutant:
    def __init__(self, ident, title, old, new, killer):
        self.ident, self.title, self.old, self.new, self.killer = ident, title, old, new, killer


MUTANTS = [
    # --- the decoder (decode_one, subst, decode_special, the block loop and the mask pass)
    Mutant("D1", "an escape that ends the input is not decoded",
           "\t\tif (end - p >= 2) {", "\t\tif (end - p >= 3) {", SPEC),
    Mutant("D2", "only the first hex digit is checked",
           "\t\t\tif ((hi | lo) != 0xff) {", "\t\t\tif (hi != 0xff) {", SPEC),
    Mutant("D3", "the decoded byte is built with the wrong shift",
           "c = static_cast<char>(hi << 4 | lo);", "c = static_cast<char>(hi << 3 | lo);", SPEC),
    Mutant("D4", "a decoded comma becomes `encoded`",
           "if (m.encoded && c != '{' && c != ',' && c != '}') { c = m.encoded; }",
           "if (m.encoded && c != '{' && c != '}') { c = m.encoded; }", SPEC),
    Mutant("D5", "`encoded` applies when it is zero",
           "if (m.encoded && c != '{'", "if (c != '{'", SPEC),
    Mutant("D6", "the plain byte of an escape stays a '%'",
           "\t\t\t\tplain = c;\n\t\t\t\tif (m.encoded", "\t\t\t\tif (m.encoded", SPEC),
    Mutant("D7", "';' becomes the substitute of '&'",
           "\t\tcase ';': return m.colon;", "\t\tcase ';': return m.amp;", SPEC),
    Mutant("D8", "'+' becomes the substitute of '&'",
           "\t\tcase '+': return m.plus;", "\t\tcase '+': return m.amp;", SPEC),
    Mutant("D9", "the plain buffer maps '=' as ';'",
           "Subst{' ', '&', ';', '=', '\\0'}", "Subst{' ', '&', ';', ';', '\\0'}", SPEC),
    Mutant("D10", "a remapped '=' alone does not select the five-needle scan",
           "m.amp != '&' || m.colon != ';' || m.eq != '='", "m.amp != '&' || m.colon != ';'", SPEC),
    Mutant("D11", "'=' is not a needle of the five-needle scan",
           "{'%', '+', '&', ';', '='}", "{'%', '+', '&', ';', ';'}", SPEC),
    Mutant("D12", "the mask pass copies a run from the start of the block",
           "const size_t run = static_cast<size_t>(s - src);", "const size_t run = static_cast<size_t>(s - block);", SPEC),
    Mutant("D13", "the mask pass drops a byte at the end of a block",
           "o += block + 16 - src;", "o += block + 15 - src;", SPEC),
    Mutant("D14", "the block loop advances the output one byte short of the special",
           "\t\t\ts = p + k;\n\t\t\to += k;", "\t\t\ts = p + k;\n\t\t\to += k - 1;", SPEC),
    # --- the vector scans of the parsers' next()
    Mutant("S1", "the name scan skips '='",
           "find_byte3<'\\0', '\\1', '\\1'>(n1, nf)", "find_byte3<'\\0', '\\0', '\\0'>(n1, nf)", NEXT),
    Mutant("S2", "the value scan skips the end of the value",
           "find_byte3<'\\0', '\\0', '\\0'>(v1, nf)", "find_byte3<'\\1', '\\1', '\\1'>(v1, nf)", NEXT),
    Mutant("S3", "the path scan skips '@'",
           "find_byte3<'\\0', ',', '@'>(n1, nf)", "find_byte3<'\\0', ',', ','>(n1, nf)", NEXT),
    Mutant("S4", "the name scan starts one byte late",
           "n1 = find_byte3<'\\0', '\\1', '\\1'>(n1, nf);", "n1 = find_byte3<'\\0', '\\1', '\\1'>(n1 + 1, nf);", NEXT),
    Mutant("S5", "the path scan starts one byte late",
           "n1 = find_byte3<'\\0', ',', '@'>(n1, nf);", "n1 = find_byte3<'\\0', ',', '@'>(n1 + 1, nf);", NEXT),
    # --- normalize_path and the plain-path test
    Mutant("N1", "a '.' component is kept",
           "\t\t\tif (size == 1) {\n\t\t\t\tcontinue;\n\t\t\t}", "\t\t\tif (size == 99) {\n\t\t\t\tcontinue;\n\t\t\t}", NORMALIZE),
    Mutant("N2", "any two-byte component that starts with a dot is a '..'",
           "if (size == 2 && name[1] == '.') {", "if (size == 2) {", NORMALIZE),
    Mutant("N3", "a '..' at the start of a relative path is dropped",
           "\t\t\t\tif (dst > floor) {", "\t\t\t\tif (dst >= floor) {", NORMALIZE),
    Mutant("N4", "the root does not absorb a '..'",
           "\t\t\t\tif (absolute) {\n\t\t\t\t\tcontinue;\n\t\t\t\t}", "", NORMALIZE),
    Mutant("N5", "the empty result is empty, not '.'",
           "\tif (dst == ret) {\n\t\t*dst++ = '.';\n\t}", "", NORMALIZE),
    Mutant("N6", "a popped component leaves its '/'",
           "dst = last > floor ? last - 1 : floor;", "dst = last;", NORMALIZE),
    Mutant("N7", "a '..' that cannot pop does not become the floor",
           "\t\t\t\tfloor = p;  // a relative path keeps a \"..\" it cannot pop\n", "", NORMALIZE),
    Mutant("N8", "`slashed` adds no slash",
           "(keep_slash ? had_slash : slashed)", "(keep_slash ? had_slash : false)", NORMALIZE),
    Mutant("N9", "a '//' path counts as plain",
           "(c == '/' && prev == '/')", "(c == '/' && prev == '.')", NORMALIZE),
]


def selected(spec):
    if not spec:
        return MUTANTS
    wanted = spec.split(",")
    return [m for m in MUTANTS if any(m.ident == w or (w.isalpha() and m.ident.startswith(w)) for w in wanted)]


def check_anchors():
    text = (ROOT / SOURCE).read_text()
    status = 0
    for m in MUTANTS:
        count = text.count(m.old)
        if count != 1:
            print(f"{m.ident}: anchor occurs {count} times in {SOURCE}", file=sys.stderr)
            status = 1
        if m.old == m.new:
            print(f"{m.ident}: replacement equals the anchor", file=sys.stderr)
            status = 1
    if status == 0:
        print(f"all {len(MUTANTS)} anchors occur exactly once")
    return status


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def failed_tests(output):
    return re.findall(r"^\s*\d+ - (\S+) \((?:Failed|Timeout|SEGFAULT|Subprocess aborted|Exception)", output, re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify the anchors only; no build")
    parser.add_argument("--list", action="store_true", help="print the mutants")
    parser.add_argument("--only", help="comma-separated ids or group letters, such as D1,S")
    parser.add_argument("--jobs", type=int, default=4, help="parallel build jobs (default 4)")
    parser.add_argument("--keep", action="store_true", help="keep the scratch copy and print its path")
    parser.add_argument("--cmake-arg", action="append", default=[], help="extra configure argument, repeatable (a compiler: -DCMAKE_CXX_COMPILER=clang++)")
    args = parser.parse_args()

    if args.check:
        return check_anchors()
    chosen = selected(args.only)
    if not chosen:
        print("no mutant matches --only", file=sys.stderr)
        return 2
    if args.list:
        for m in chosen:
            print(f"{m.ident:4} {m.killer:22} {m.title}")
        return 0
    if check_anchors():
        return 2

    work = Path(tempfile.mkdtemp(prefix="url-parser-mutants-"))
    sources = work / "src"
    try:
        shutil.copytree(ROOT, sources, ignore=shutil.ignore_patterns(".git", "build*", "docs"))
        build = work / "build"
        configure = ["cmake", "-S", str(sources), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"] + args.cmake_arg
        result = run(configure, work)
        if result.returncode:
            print(result.stdout)
            return 2

        def build_and_test():
            built = run(["cmake", "--build", str(build), "--parallel", str(args.jobs)], work)
            if built.returncode:
                return "build error", [], built.stdout
            # The timing guard is not a correctness test; the anchors test reads the mutated file.
            ctest = ["ctest", "--test-dir", str(build), "--output-on-failure", "--timeout", "600",
                     "-E", "url_parser_perf|url_parser_mutants_anchors"]
            tested = run(ctest, work)
            if tested.returncode == 0:
                return "survived", [], tested.stdout
            failed = failed_tests(tested.stdout)
            # Killed only if the failure repeats.
            again = run(["ctest", "--test-dir", str(build), "--output-on-failure", "--timeout", "600",
                         "-R", "^(" + "|".join(failed) + ")$"], work) if failed else tested
            return ("killed" if again.returncode else "not reproduced"), failed, tested.stdout

        outcome, failed, output = build_and_test()
        if outcome != "survived":
            print(f"the unmutated suite does not pass ({outcome}: {' '.join(failed)}):\n{output[-4000:]}")
            return 2
        print("unmutated suite passes", flush=True)

        original = (sources / SOURCE).read_text()
        bad = 0
        for m in chosen:
            (sources / SOURCE).write_text(original.replace(m.old, m.new, 1))
            try:
                outcome, failed, output = build_and_test()
            finally:
                (sources / SOURCE).write_text(original)
            ok = outcome == "killed" and m.killer in failed
            bad += not ok
            print(f"{m.ident:4} {outcome:11} {'ok' if ok else 'UNEXPECTED':10} {m.title}"
                  f" (names {m.killer}; failed: {', '.join(failed) or 'none'})", flush=True)
            if outcome == "build error":
                print(output[-1500:])
        print(f"{len(chosen)} mutants, {bad} not killed by the test they name")
        return 1 if bad else 0
    finally:
        if args.keep:
            print(f"scratch copy: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
