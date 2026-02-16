#!/usr/bin/env python3
"""lint.py — Enforce project-specific coding conventions from CONTRIBUTION_GUIDELINES.md.

Usage:
    python tools/lint.py                  # Check all src/**/*.h and src/**/*.cpp
    python tools/lint.py src/core/ray.h   # Check specific files
    python tools/lint.py --fix            # Auto-fix what's possible (header format only)

Exit codes:
    0  All checks passed
    1  One or more violations found
"""

import argparse
import os
import re
import sys
from pathlib import Path
from typing import List, Tuple, NamedTuple

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Directories relative to project root
SRC_DIR = "src"

# Files/directories to skip entirely
SKIP_DIRS = {"gen", "__pycache__"}
SKIP_FILES = {"register_types.h", "register_types.cpp", "example_class.h", "example_class.cpp"}
SKIP_SUFFIXES = {".gen.h", ".gen.cpp", ".obj"}

# Assertion macros we recognize
ASSERTION_MACROS = {
    "RT_ASSERT", "RT_VERIFY", "RT_SLOW_ASSERT",
    "RT_ASSERT_VALID_RAY", "RT_ASSERT_FINITE", "RT_ASSERT_NOT_NULL",
    "RT_ASSERT_BOUNDS", "RT_ASSERT_BOUNDS_U", "RT_ASSERT_POSITIVE",
    "RT_ASSERT_NORMALIZED", "RT_UNREACHABLE",
}

# Forbidden includes from modules/ directory
FORBIDDEN_MODULE_INCLUDES = {
    "raytracer_server.h",
    "accel/bvh.h",
    "accel/mesh_blas.h",
    "accel/scene_tlas.h",
    "accel/blas_instance.h",
    "dispatch/ray_dispatcher.h",
    "dispatch/ray_sort.h",
    "gpu/gpu_ray_caster.h",
}

# GPU struct name patterns that require static_assert
GPU_STRUCT_PATTERN = re.compile(r"^struct\s+(GPU\w+(?:Packed|Wide|Constants))\s*\{", re.MULTILINE)

# Minimum assertion count for non-trivial functions
MIN_ASSERTIONS_PER_FUNCTION = 2

# Minimum lines for a function body to be considered "non-trivial"
MIN_FUNCTION_BODY_LINES = 8

# ---------------------------------------------------------------------------
# Violation tracking
# ---------------------------------------------------------------------------

class Violation(NamedTuple):
    file: str
    line: int
    rule: str
    message: str


def rel_path(path: str, root: str) -> str:
    """Return a display-friendly path relative to the project root."""
    try:
        return str(Path(path).relative_to(root))
    except ValueError:
        return path


# ---------------------------------------------------------------------------
# Check functions
# ---------------------------------------------------------------------------

def check_pragma_once(filepath: str, lines: List[str]) -> List[Violation]:
    """Header files must start with #pragma once."""
    if not filepath.endswith(".h"):
        return []
    if not lines or lines[0].strip() != "#pragma once":
        return [Violation(filepath, 1, "header/pragma-once",
                          "Header files must start with '#pragma once' on line 1")]
    return []


def check_file_header(filepath: str, lines: List[str]) -> List[Violation]:
    """Description comment must be '// filename — Description'.

    For .h files: line 2 (after #pragma once).
    For .cpp files: line 1 (no #pragma once).
    """
    violations = []
    basename = os.path.basename(filepath)

    # For headers, description is on line 2 (after #pragma once).
    # For source files, description is on line 1.
    if filepath.endswith(".h"):
        check_line_idx = 1
        check_line_num = 2
    else:
        check_line_idx = 0
        check_line_num = 1

    if len(lines) <= check_line_idx:
        violations.append(Violation(filepath, 1, "header/description",
                                    f"File must have a header description comment"))
        return violations

    target_line = lines[check_line_idx]
    # Check for the pattern: // filename.ext — some description
    # Allow both em-dash (\u2014) and double-hyphen (--) for flexibility
    pattern = re.compile(
        r"^//\s*" + re.escape(basename) + r"\s*[\u2014\-]+\s*\S",
    )
    if not pattern.match(target_line):
        violations.append(Violation(filepath, check_line_num, "header/description",
                                    f"Line {check_line_num} must be '// {basename} \u2014 <description>'"))
    return violations


def check_gpu_struct_static_assert(filepath: str, content: str) -> List[Violation]:
    """Every GPU*Packed/Wide/Constants struct must have a static_assert on sizeof."""
    violations = []
    for match in GPU_STRUCT_PATTERN.finditer(content):
        struct_name = match.group(1)
        line_num = content[:match.start()].count("\n") + 1

        # Look for static_assert(sizeof(StructName) after the struct definition
        assert_pattern = re.compile(
            r"static_assert\s*\(\s*sizeof\s*\(\s*" + re.escape(struct_name) + r"\s*\)"
        )
        if not assert_pattern.search(content):
            violations.append(Violation(filepath, line_num, "gpu/static-assert",
                                        f"GPU struct '{struct_name}' missing static_assert(sizeof(...))"))
    return violations


def check_module_include_boundary(filepath: str, lines: List[str]) -> List[Violation]:
    """Files in src/modules/ must not include server internals."""
    # Normalize path separators
    norm_path = filepath.replace("\\", "/")
    if "/modules/" not in norm_path:
        return []

    violations = []
    include_pattern = re.compile(r'^\s*#include\s*[<"]([^>"]+)[>"]')

    for i, line in enumerate(lines, 1):
        m = include_pattern.match(line)
        if not m:
            continue
        included = m.group(1)
        for forbidden in FORBIDDEN_MODULE_INCLUDES:
            if included.endswith(forbidden) or included == forbidden:
                violations.append(Violation(filepath, i, "module/boundary",
                                            f"Modules must not include '{forbidden}' — "
                                            "use api/ray_service.h instead"))
    return violations


def extract_function_bodies(lines: List[str]) -> List[Tuple[str, int, int, List[str]]]:
    """
    Extract function bodies using brace matching.
    Returns list of (function_signature, start_line, end_line, body_lines).

    This is a heuristic parser — not a full C++ parser.
    It looks for lines that look like function definitions (have parentheses,
    followed by opening brace, not in a struct/class/enum/namespace context).
    """
    functions = []

    # Patterns for things that are NOT standalone function definitions
    not_function = re.compile(
        r"^\s*(?:struct|class|enum|namespace|if|else|for|while|switch|do|try|catch"
        r"|using|typedef|template|public|private|protected|static_assert|#|//|/\*|\*)"
    )

    # Pattern for function-like definition (type name(params) { or const {)
    # Simplified: look for identifier followed by ( ... ) possibly followed by const/override/= 0
    # And then { on same line or next line
    func_sig = re.compile(
        r"^[^#/]*\b(\w+)\s*\([^;]*\)\s*(?:const\s*)?(?:override\s*)?(?:->.*?)?\s*\{?\s*$"
    )

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        # Skip obvious non-functions
        if not_function.match(stripped):
            i += 1
            continue

        # Skip blank lines and pure comments
        if not stripped or stripped.startswith("//") or stripped.startswith("/*"):
            i += 1
            continue

        m = func_sig.match(stripped)
        if not m:
            i += 1
            continue

        func_name = m.group(1)

        # Skip macros and certain keywords
        if func_name in ("static_assert", "sizeof", "alignof", "decltype",
                         "if", "while", "for", "switch", "return", "do",
                         "RT_ASSERT", "RT_VERIFY", "RT_SLOW_ASSERT",
                         "RT_ASSERT_FAIL_", "RT_UNREACHABLE",
                         # Godot boilerplate — these are just ClassDB::bind calls
                         "_bind_methods", "_validate_property",
                         # Destructors and worker loops are hard to assert meaningfully
                         "void"):
            i += 1
            continue

        # Find the opening brace
        brace_line = i
        if "{" not in stripped:
            # Check next line for opening brace
            if i + 1 < len(lines) and lines[i + 1].strip().startswith("{"):
                brace_line = i + 1
            else:
                i += 1
                continue

        # Count braces to find the end
        depth = 0
        body_start = brace_line
        body_lines = []
        j = brace_line
        found_body = False
        while j < len(lines):
            for ch in lines[j]:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        found_body = True
                        break
            body_lines.append(lines[j])
            if found_body:
                break
            j += 1

        if found_body and len(body_lines) >= MIN_FUNCTION_BODY_LINES:
            functions.append((func_name, i + 1, j + 1, body_lines))

        i = j + 1 if found_body else i + 1

    return functions


def check_assertion_density(filepath: str, lines: List[str]) -> List[Violation]:
    """Non-trivial functions should have at least 2 assertions."""
    violations = []

    # Only check .h and .cpp files in src/ (not generated, not third-party)
    norm_path = filepath.replace("\\", "/")
    if "/gen/" in norm_path or "/godot-cpp/" in norm_path:
        return []

    # Skip the asserts.h file itself (it defines the macros, doesn't use them normally)
    if os.path.basename(filepath) == "asserts.h":
        return []

    functions = extract_function_bodies(lines)
    assertion_re = re.compile(r"\b(" + "|".join(ASSERTION_MACROS) + r")\b")

    for func_name, start_line, end_line, body_lines in functions:
        # Skip trivial functions: getters, setters, constructors with only initializer
        body_text = "\n".join(body_lines)

        # Skip functions that are just returning a member or calling base
        stripped_body = [l.strip() for l in body_lines
                         if l.strip() and not l.strip().startswith("//")]
        # Remove braces-only lines
        code_lines = [l for l in stripped_body if l not in ("{", "}")]

        if len(code_lines) < MIN_FUNCTION_BODY_LINES:
            continue

        assertion_count = len(assertion_re.findall(body_text))
        if assertion_count < MIN_ASSERTIONS_PER_FUNCTION:
            violations.append(Violation(
                filepath, start_line, "tiger/assertion-density",
                f"Function '{func_name}' has {assertion_count} assertion(s), "
                f"minimum is {MIN_ASSERTIONS_PER_FUNCTION} "
                f"(Tiger Style: at least 2 per non-trivial function)"
            ))

    return violations


def check_naming_conventions(filepath: str, lines: List[str]) -> List[Violation]:
    """Basic naming checks: class/struct should be PascalCase, members trailing underscore."""
    violations = []

    # Check struct/class names are PascalCase
    class_pattern = re.compile(r"^\s*(?:class|struct)\s+(\w+)\s*[:{]")
    for i, line in enumerate(lines, 1):
        m = class_pattern.match(line)
        if not m:
            continue
        name = m.group(1)
        # Skip GPU-prefixed (already PascalCase by convention)
        # Skip all-caps names (might be a macro-defined struct)
        if name.isupper() or name.startswith("_"):
            continue
        # PascalCase: first letter uppercase, no leading lowercase
        # Allow GPU prefix and I prefix for interfaces (IFoo pattern)
        # For interface check: only strip I if followed by uppercase (IRayService, not Intersection)
        clean = name
        if len(name) > 1 and name[0] == "I" and name[1].isupper():
            clean = name[1:]  # IFoo -> Foo (interface prefix)
        if clean and not clean[0].isupper():
            violations.append(Violation(filepath, i, "naming/class-pascal",
                                        f"Class/struct '{name}' should be PascalCase"))

    return violations


# ---------------------------------------------------------------------------
# File discovery
# ---------------------------------------------------------------------------

def should_skip(filepath: str) -> bool:
    """Check if a file should be skipped."""
    basename = os.path.basename(filepath)
    norm = filepath.replace("\\", "/")

    if basename in SKIP_FILES:
        return True
    for suffix in SKIP_SUFFIXES:
        if basename.endswith(suffix):
            return True
    for skip_dir in SKIP_DIRS:
        if f"/{skip_dir}/" in norm or norm.endswith(f"/{skip_dir}"):
            return True
    if "/godot-cpp/" in norm:
        return True
    return False


def find_source_files(root: str) -> List[str]:
    """Find all .h and .cpp files under src/."""
    src_root = os.path.join(root, SRC_DIR)
    files = []
    for dirpath, dirnames, filenames in os.walk(src_root):
        for f in sorted(filenames):
            if f.endswith((".h", ".cpp")):
                full = os.path.join(dirpath, f)
                if not should_skip(full):
                    files.append(full)
    return files


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def lint_file(filepath: str) -> List[Violation]:
    """Run all checks on a single file."""
    try:
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
    except OSError as e:
        return [Violation(filepath, 0, "io/read-error", str(e))]

    lines = content.splitlines()

    violations = []
    violations.extend(check_pragma_once(filepath, lines))
    violations.extend(check_file_header(filepath, lines))
    violations.extend(check_gpu_struct_static_assert(filepath, content))
    violations.extend(check_module_include_boundary(filepath, lines))
    violations.extend(check_assertion_density(filepath, lines))
    violations.extend(check_naming_conventions(filepath, lines))
    return violations


def main():
    parser = argparse.ArgumentParser(
        description="Lint project source files against CONTRIBUTION_GUIDELINES.md conventions.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Rules checked:
  header/pragma-once       Headers must start with #pragma once
  header/description       Line 2 must be '// filename — description'
  gpu/static-assert        GPU structs need static_assert(sizeof(...))
  module/boundary          Modules must not include server internals
  tiger/assertion-density  Non-trivial functions need >= 2 assertions
  naming/class-pascal      Class/struct names must be PascalCase

Examples:
  python tools/lint.py                       # Lint all source files
  python tools/lint.py src/core/ray.h        # Lint one file
  python tools/lint.py --rule header          # Only header checks
  python tools/lint.py --summary             # Show counts only
"""
    )
    parser.add_argument("files", nargs="*",
                        help="Specific files to check (default: all under src/)")
    parser.add_argument("--rule", type=str, default=None,
                        help="Only check rules matching this prefix (e.g., 'header', 'tiger', 'gpu')")
    parser.add_argument("--summary", action="store_true",
                        help="Show violation summary counts instead of individual violations")
    parser.add_argument("--quiet", action="store_true",
                        help="Only print the final count")

    args = parser.parse_args()

    # Find project root (parent of tools/)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)

    # Gather files
    if args.files:
        files = [os.path.abspath(f) for f in args.files]
    else:
        files = find_source_files(project_root)

    if not files:
        print("No source files found.")
        return 0

    # Run checks
    all_violations: List[Violation] = []
    for filepath in files:
        violations = lint_file(filepath)
        if args.rule:
            violations = [v for v in violations if v.rule.startswith(args.rule)]
        all_violations.extend(violations)

    # Report
    if args.summary:
        counts: dict = {}
        for v in all_violations:
            counts[v.rule] = counts.get(v.rule, 0) + 1
        if counts:
            print(f"\n{'Rule':<30} {'Count':>6}")
            print("-" * 38)
            for rule, count in sorted(counts.items()):
                print(f"  {rule:<28} {count:>6}")
            print("-" * 38)
            print(f"  {'TOTAL':<28} {len(all_violations):>6}")
        else:
            print("All checks passed!")
        print(f"\nFiles checked: {len(files)}")
    elif not args.quiet:
        for v in sorted(all_violations, key=lambda v: (v.file, v.line)):
            rel = rel_path(v.file, project_root)
            print(f"  {rel}:{v.line}: [{v.rule}] {v.message}")
        if all_violations:
            print(f"\n{len(all_violations)} violation(s) in {len(files)} file(s)")
        else:
            print(f"All checks passed! ({len(files)} files)")
    else:
        if all_violations:
            print(f"{len(all_violations)} violation(s)")
        else:
            print("0 violations")

    return 1 if all_violations else 0


if __name__ == "__main__":
    sys.exit(main())
