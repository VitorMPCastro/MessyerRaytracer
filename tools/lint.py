#!/usr/bin/env python3
"""lint.py — Project convention linter for MessyerRaytracer.

Enforces conventions documented in CONTRIBUTION_GUIDELINES.md that no
off-the-shelf tool can check:

  RULE FAMILIES
  ─────────────────────────────────────────────────────────────────
  header/          File header conventions (#pragma once, description)
  tiger/           Assertion density (≥2 per non-trivial function)
  gpu/             GPU struct static_assert + GLSL mirror
  module/          Module dependency boundary enforcement
  naming/          PascalCase classes, snake_case functions, etc.
  godot-native/    Godot-Native Principle enforcement ★

  ★ The godot-native/ family is unique to this project.  It prevents
    "parallel state" — C++ members or GDScript properties that duplicate
    values already available on Godot scene nodes (DirectionalLight3D,
    WorldEnvironment, Camera3D, etc.).

USAGE
  python tools/lint.py                     # lint all source files
  python tools/lint.py src/core/ray.h      # lint specific files
  python tools/lint.py --rule godot-native # only godot-native rules
  python tools/lint.py --summary           # rule-by-rule counts
  python tools/lint.py --verbose           # show passing files too

SUPPRESSION
  Inline:   // rt-lint: suppress godot-native/parallel-state-cpp
  Inline:   # rt-lint: suppress godot-native/scene-property-gd
  File:     Listed in tools/lint.conf under [suppress] section

EXIT CODES
  0  All checks pass
  1  One or more violations found
  2  Configuration / argument error
"""

from __future__ import annotations

import argparse
import re
import sys
import textwrap
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ──────────────────────────────────────────────────────────────────────
#  Configuration
# ──────────────────────────────────────────────────────────────────────

# Directories to scan (relative to project root).
SRC_DIR = "src"
DEMO_DIR = "project/demos"

# Files / directories to skip entirely.
SKIP_DIRS = {"gen", "thirdparty", "godot-cpp", "__pycache__", ".git", "bin"}
SKIP_FILES = {"doc_data.gen.cpp"}

# File suffixes that indicate auto-generated code (skip entirely).
SKIP_SUFFIXES = (".gen.h", ".gen.cpp")

# Minimum non-trivial function body length that requires 2+ assertions.
MIN_FUNCTION_BODY_LINES = 5

# ──────────────────────────────────────────────────────────────────────
#  Data types
# ──────────────────────────────────────────────────────────────────────

@dataclass
class Violation:
    """A single lint violation."""
    file: str
    line: int
    rule: str
    message: str
    severity: str = "error"  # "error" | "warning"

    def __str__(self) -> str:
        return f"{self.file}:{self.line}: {self.severity.capitalize()}: {self.message} ({self.rule})"


@dataclass
class LintStats:
    """Aggregate statistics for a lint run."""
    files_checked: int = 0
    files_passed: int = 0
    violations: list[Violation] = field(default_factory=list)
    suppressed: int = 0

    @property
    def files_failed(self) -> int:
        return self.files_checked - self.files_passed

    def rule_counts(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for v in self.violations:
            counts[v.rule] = counts.get(v.rule, 0) + 1
        return dict(sorted(counts.items()))


# ──────────────────────────────────────────────────────────────────────
#  Suppression config
# ──────────────────────────────────────────────────────────────────────

# Inline suppression pattern:  // rt-lint: suppress <rule>
#                              # rt-lint: suppress <rule>
_SUPPRESS_RE = re.compile(
    r"(?://|#)\s*rt-lint:\s*suppress\s+([\w\-/]+)"
)

def _load_file_suppressions(root: Path) -> dict[str, set[str]]:
    """Load per-file suppressions from tools/lint.conf.

    Format:
        [suppress]
        src/modules/graphics/rt_compositor_base.cpp = module/boundary
    """
    conf = root / "tools" / "lint.conf"
    result: dict[str, set[str]] = {}
    if not conf.exists():
        return result

    in_suppress = False
    for raw_line in conf.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line.startswith("[") and line.endswith("]"):
            in_suppress = (line.lower() == "[suppress]")
            continue
        if not in_suppress or not line or line.startswith("#"):
            continue
        if "=" in line:
            path_str, _, rules_str = line.partition("=")
            path_str = path_str.strip()
            for rule in rules_str.split(","):
                rule = rule.strip()
                if rule:
                    result.setdefault(path_str, set()).add(rule)
    return result


# ══════════════════════════════════════════════════════════════════════
#  RULE IMPLEMENTATIONS
# ══════════════════════════════════════════════════════════════════════

# ──────────────────────────────────────────────────────────────
#  header/  — File header conventions
# ──────────────────────────────────────────────────────────────

def check_header_pragma_once(path: str, lines: list[str]) -> list[Violation]:
    """header/pragma-once — Headers must start with #pragma once."""
    if not path.endswith(".h"):
        return []
    if not lines or lines[0].strip() != "#pragma once":
        return [Violation(path, 1, "header/pragma-once",
                          "Header must start with '#pragma once'")]
    return []


def check_header_description(path: str, lines: list[str]) -> list[Violation]:
    """header/description — Line 2 must be '// filename — description'."""
    if not path.endswith(".h"):
        return []
    if len(lines) < 2:
        return [Violation(path, 2, "header/description",
                          "Missing file description on line 2")]
    filename = Path(path).name
    line2 = lines[1].strip()
    # Accept: // filename.h — description  OR  // filename — description
    # also accept em-dash (—) or double-dash (--)
    stem = filename.replace(".h", "")
    if not (line2.startswith(f"// {filename}") or line2.startswith(f"// {stem}")):
        return [Violation(path, 2, "header/description",
                          f"Line 2 should be '// {filename} — description'")]
    return []


# ──────────────────────────────────────────────────────────────
#  tiger/  — Assertion density
# ──────────────────────────────────────────────────────────────

# Matches ALL assertion macros: RT_ASSERT, RT_VERIFY, RT_SLOW_ASSERT, RT_UNREACHABLE,
# and convenience macros: RT_ASSERT_VALID_RAY, RT_ASSERT_FINITE, RT_ASSERT_NOT_NULL,
# RT_ASSERT_BOUNDS, RT_ASSERT_BOUNDS_U, RT_ASSERT_POSITIVE, RT_ASSERT_NORMALIZED,
# RT_ASSERT_INDEX.  Uses \b only at the start to avoid missing suffixed variants.
_ASSERT_RE = re.compile(r"\bRT_(ASSERT|VERIFY|SLOW_ASSERT|UNREACHABLE)\w*")

# Matches function definitions (not declarations): return_type name(...) {
_FUNC_DEF_RE = re.compile(
    r"^(?!.*;\s*$)"                   # not a declaration (no trailing ;)
    r"(?!.*#)"                        # not a preprocessor line
    r"\s*"
    r"(?:(?:static|inline|virtual|constexpr|const|explicit)\s+)*"
    r"(?:[\w:*&<>,\s]+\s+)"          # return type (rough)
    r"([\w:~]+)"                      # function name
    r"\s*\([^)]*\)"                   # parameter list
    r"(?:\s*(?:const|override|noexcept|final))*"
    r"\s*\{"                          # opening brace
)

# C++ keywords that the function regex might accidentally match.
_CPP_KEYWORDS = frozenset({
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "return", "break", "continue", "goto", "catch", "throw", "try",
    "new", "delete", "sizeof", "alignof", "decltype", "typeid",
    "static_assert", "static_cast", "dynamic_cast", "const_cast",
    "reinterpret_cast", "co_await", "co_return", "co_yield",
})

def check_tiger_assertion_density(path: str, lines: list[str]) -> list[Violation]:
    """tiger/assertion-density — Non-trivial functions need ≥2 assertions."""
    if not (path.endswith(".h") or path.endswith(".cpp")):
        return []
    violations = []
    i = 0
    while i < len(lines):
        m = _FUNC_DEF_RE.match(lines[i])
        if not m:
            i += 1
            continue

        func_name = m.group(1)
        # Extract unqualified name (strip Class:: prefix).
        unqualified = func_name.split("::")[-1]
        # Skip C++ keywords that the regex accidentally matched.
        if unqualified in _CPP_KEYWORDS:
            i += 1
            continue
        # Skip trivial functions: getters, setters, constructors, bind_methods,
        # operators, notification dispatchers, and empty virtual hooks.
        _TRIVIAL_PREFIXES = ("get_", "set_", "is_", "has_", "_bind_methods",
                             "operator", "_notification")
        if any(unqualified.startswith(p) for p in _TRIVIAL_PREFIXES):
            i += 1
            continue

        # Find the matching closing brace.
        # Start brace_depth = 1 (for the opening '{' on the definition line).
        # Must also count any additional braces on the definition line itself
        # (handles one-liner functions: "int foo() { return x; }").
        brace_depth = 0
        for ch in lines[i]:
            if ch == '{':
                brace_depth += 1
            elif ch == '}':
                brace_depth -= 1

        # If the function opens and closes on the same line, skip it.
        if brace_depth <= 0:
            i += 1
            continue

        body_start = i + 1
        j = body_start
        while j < len(lines) and brace_depth > 0:
            for ch in lines[j]:
                if ch == '{':
                    brace_depth += 1
                elif ch == '}':
                    brace_depth -= 1
                    if brace_depth == 0:
                        break
            j += 1
        body_end = j

        body_lines = body_end - body_start
        if body_lines < MIN_FUNCTION_BODY_LINES:
            i = body_end
            continue

        # Count assertions in the body.
        assert_count = sum(
            1 for k in range(body_start, min(body_end, len(lines)))
            if _ASSERT_RE.search(lines[k])
        )
        if assert_count < 2:
            violations.append(Violation(
                path, i + 1, "tiger/assertion-density",
                f"Function '{func_name}' has {assert_count} assertion(s), "
                f"need ≥2 (body is {body_lines} lines)",
                severity="warning"
            ))

        i = body_end
    return violations


# ──────────────────────────────────────────────────────────────
#  gpu/  — GPU struct rules
# ──────────────────────────────────────────────────────────────

# Matches GPU struct definitions (not forward declarations ending with ;).
_GPU_STRUCT_DEF_RE = re.compile(r"\bstruct\s+(GPU\w+Packed)\b[^;]*\{")

def check_gpu_static_assert(path: str, lines: list[str]) -> list[Violation]:
    """gpu/static-assert — GPU structs need static_assert(sizeof(...))."""
    if not (path.endswith(".h") or path.endswith(".cpp")):
        return []
    violations = []
    full_text = "\n".join(lines)
    for m in _GPU_STRUCT_DEF_RE.finditer(full_text):
        struct_name = m.group(1)
        if f"static_assert(sizeof({struct_name})" not in full_text:
            # Find the line number of the struct definition.
            line_no = full_text[:m.start()].count('\n') + 1
            violations.append(Violation(
                path, line_no, "gpu/static-assert",
                f"GPU struct '{struct_name}' needs static_assert(sizeof(...))"
            ))
    return violations


# ──────────────────────────────────────────────────────────────
#  module/  — Module dependency boundary
# ──────────────────────────────────────────────────────────────

# Files in modules/ must not include from godot/ or other internal modules.
_FORBIDDEN_INCLUDES = {
    "modules/": [
        re.compile(r'#include\s*"godot/'),
        re.compile(r'#include\s*"raytracer_server\.h"'),
        re.compile(r'#include\s*"dispatch/thread_pool\.h"'),
        re.compile(r'#include\s*"accel/'),
    ]
}

def check_module_boundary(path: str, lines: list[str]) -> list[Violation]:
    """module/boundary — Modules must not include server internals."""
    if not (path.endswith(".h") or path.endswith(".cpp")):
        return []
    violations = []
    for module_prefix, patterns in _FORBIDDEN_INCLUDES.items():
        if module_prefix not in path.replace("\\", "/"):
            continue
        for i, line in enumerate(lines):
            for pat in patterns:
                if pat.search(line):
                    violations.append(Violation(
                        path, i + 1, "module/boundary",
                        f"Module file must not include internal header: {line.strip()}"
                    ))
    return violations


# ──────────────────────────────────────────────────────────────
#  naming/  — Naming conventions
# ──────────────────────────────────────────────────────────────

_CLASS_DECL_RE = re.compile(r"^\s*(?:class|struct)\s+(\w+)\s*[:{]")
_PASCAL_RE = re.compile(r"^[A-Z][a-zA-Z0-9]*$")
# Allow GPU-prefixed names and data-transfer structs
_NAMING_EXEMPT = {"GDCLASS", "VARIANT_ENUM_CAST"}

def check_naming_class_pascal(path: str, lines: list[str]) -> list[Violation]:
    """naming/class-pascal — Class/struct names must be PascalCase."""
    if not path.endswith(".h"):
        return []
    violations = []
    for i, line in enumerate(lines):
        m = _CLASS_DECL_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        # Skip forward declarations and internal/anonymous structs
        if name.startswith("_") or name in _NAMING_EXEMPT:
            continue
        if not _PASCAL_RE.match(name):
            violations.append(Violation(
                path, i + 1, "naming/class-pascal",
                f"Class/struct '{name}' should be PascalCase"
            ))
    return violations


# ══════════════════════════════════════════════════════════════════════
#  GODOT-NATIVE RULES  ★
# ══════════════════════════════════════════════════════════════════════
#
# These rules enforce the Godot-Native Principle:
#   "Read from the scene tree. Never maintain parallel state."
#
# They detect C++ member variables and GDScript properties that duplicate
# values already available on native Godot scene nodes.

# ── C++ detection ────────────────────────────────────────────────────

# Member variables whose names indicate they shadow a Godot node property.
# Mapping: regex pattern → (Godot node type, what to read instead)
_GODOT_OWNED_MEMBERS: list[tuple[re.Pattern, str, str]] = [
    # Sun / DirectionalLight3D
    (re.compile(r"\bsun_direction_\b"),       "DirectionalLight3D", "-transform.basis.get_column(2)"),
    (re.compile(r"\bsun_color_\b"),           "DirectionalLight3D", "get_color()"),
    (re.compile(r"\bsun_energy_\b"),          "DirectionalLight3D", "get_param(PARAM_ENERGY)"),
    (re.compile(r"\bsun_rotation_degrees_\b"),"DirectionalLight3D", "rotation_degrees"),
    (re.compile(r"\bsun_shadows_\b"),         "DirectionalLight3D", "shadow_enabled"),
    (re.compile(r"\bsun_shadow_cascades_\b"), "DirectionalLight3D", "directional_shadow_max_distance"),
    (re.compile(r"\bsun_shadow_max_distance_\b"), "DirectionalLight3D", "directional_shadow_max_distance"),
    # Sky / ProceduralSkyMaterial
    (re.compile(r"\bsky_top_color_\b"),       "ProceduralSkyMaterial", "sky_top_color"),
    (re.compile(r"\bsky_horizon_color_\b"),   "ProceduralSkyMaterial", "sky_horizon_color"),
    (re.compile(r"\bsky_bottom_color_\b"),    "ProceduralSkyMaterial", "ground_bottom_color"),
    (re.compile(r"\bsky_energy_\b"),          "Sky / Environment", "sky.sky_material.energy_multiplier"),
    # Ambient / Environment
    (re.compile(r"\bambient_color_\b"),       "Environment", "ambient_light_color"),
    (re.compile(r"\bambient_energy_\b"),      "Environment", "ambient_light_energy"),
    # Tonemapping / Environment
    (re.compile(r"\btonemap_mode_\b"),        "Environment", "tonemap_mode"),
    (re.compile(r"\btonemap_exposure_\b"),    "Environment", "tonemap_exposure"),
    (re.compile(r"\btonemap_white_\b"),       "Environment", "tonemap_white"),
    # SSR / Environment
    (re.compile(r"\bssr_enabled_\b"),         "Environment", "ssr_enabled"),
    (re.compile(r"\bssr_max_steps_\b"),       "Environment", "ssr_max_steps"),
    (re.compile(r"\bssr_fade_in_\b"),         "Environment", "ssr_fade_in"),
    (re.compile(r"\bssr_fade_out_\b"),        "Environment", "ssr_fade_out"),
    (re.compile(r"\bssr_depth_tolerance_\b"), "Environment", "ssr_depth_tolerance"),
    # SSAO / Environment
    (re.compile(r"\bssao_enabled_\b"),        "Environment", "ssao_enabled"),
    (re.compile(r"\bssao_radius_\b"),         "Environment", "ssao_radius"),
    (re.compile(r"\bssao_intensity_\b"),      "Environment", "ssao_intensity"),
    # SSIL / Environment
    (re.compile(r"\bssil_enabled_\b"),        "Environment", "ssil_enabled"),
    (re.compile(r"\bssil_radius_\b"),         "Environment", "ssil_radius"),
    (re.compile(r"\bssil_intensity_\b"),      "Environment", "ssil_intensity"),
    # Glow / Environment
    (re.compile(r"\bglow_enabled_\b"),        "Environment", "glow_enabled"),
    (re.compile(r"\bglow_intensity_\b"),      "Environment", "glow_intensity"),
    (re.compile(r"\bglow_bloom_\b"),          "Environment", "glow_bloom"),
    # Fog / Environment
    (re.compile(r"\bfog_enabled_\b"),         "Environment", "fog_enabled"),
    (re.compile(r"\bfog_density_\b"),         "Environment", "fog_density"),
    (re.compile(r"\bfog_color_\b"),           "Environment", "fog_aerial_perspective"),
    # DOF / CameraAttributesPractical
    (re.compile(r"\bdof_enabled_\b"),         "CameraAttributesPractical", "dof_blur_far_enabled"),
    (re.compile(r"\bdof_focus_distance_\b"),  "CameraAttributesPractical", "dof_blur_far_distance"),
    (re.compile(r"\bdof_blur_amount_\b"),     "CameraAttributesPractical", "dof_blur_amount"),
    # Auto exposure / CameraAttributesPractical
    (re.compile(r"\bauto_exposure_enabled_\b"),  "CameraAttributesPractical", "auto_exposure_enabled"),
    (re.compile(r"\bauto_exposure_scale_\b"),    "CameraAttributesPractical", "auto_exposure_scale"),
    (re.compile(r"\bauto_exposure_min_\b"),      "CameraAttributesPractical", "auto_exposure_min_sensitivity"),
    (re.compile(r"\bauto_exposure_max_\b"),      "CameraAttributesPractical", "auto_exposure_max_sensitivity"),
    # GI / Environment
    (re.compile(r"\bgi_mode_\b"),                "Environment", "gi_mode (sdfgi/voxel_gi)"),
    (re.compile(r"\bsdfgi_cascades_\b"),         "Environment", "sdfgi_cascades"),
    (re.compile(r"\bsdfgi_min_cell_size_\b"),    "Environment", "sdfgi_min_cell_size"),
    (re.compile(r"\bsdfgi_use_occlusion_\b"),    "Environment", "sdfgi_use_occlusion"),
    # Camera / Camera3D
    (re.compile(r"\bdepth_range_\b"),         "Camera3D", "get_far() - get_near()"),
    (re.compile(r"\bcamera_fov_\b"),          "Camera3D", "get_fov()"),
    (re.compile(r"\bcamera_near_\b"),         "Camera3D", "get_near()"),
    (re.compile(r"\bcamera_far_\b"),          "Camera3D", "get_far()"),
]

# Structs whose job is to *transfer* scene data to pure functions.
# They're allowed to have these member names because they don't *own* state —
# they're populated per-frame from actual Godot nodes.
_DATA_TRANSFER_STRUCTS = {
    "EnvironmentData",   # shade_pass.h — populated per frame from Environment
    "LightData",         # light_data.h — populated per frame from Light nodes
    "SceneShadeData",    # scene_shade_data.h — read-only batch of scene data
    "GPURayPacked",      # GPU transit structs
    "GPUTrianglePacked",
    "GPUBVHNodePacked",
    "GPUHitResultPacked",
    "GPUMaterialPacked",
    "GPULightPacked",
}

# Hardcoded scene values: constexpr/static const for things that belong on nodes.
_HARDCODED_SCENE_PATTERNS: list[tuple[re.Pattern, str]] = [
    (re.compile(r"\bconstexpr\b.*\b(SKY_ZENITH|SKY_HORIZON|SKY_GROUND|SUN_DIR|SUN_COLOR)\b", re.IGNORECASE),
     "Sky/sun constants should be read from scene nodes, not hardcoded"),
    (re.compile(r"\bstatic\s+(?:const(?:expr)?)\s+.*\b(AMBIENT_COLOR|AMBIENT_ENERGY)\b", re.IGNORECASE),
     "Ambient constants should be read from Environment node"),
    (re.compile(r"\bconstexpr\b.*\bDEPTH_RANGE\b", re.IGNORECASE),
     "Depth range should be read from Camera3D (get_far() - get_near())"),
]


def check_godot_native_cpp(path: str, lines: list[str]) -> list[Violation]:
    """godot-native/parallel-state-cpp — C++ members duplicating Godot node state.

    Detects member variables in C++ classes that shadow properties available
    on standard Godot scene nodes.  Exempts data-transfer structs whose
    purpose is to carry batched scene reads to pure functions.
    """
    if not (path.endswith(".h") or path.endswith(".cpp")):
        return []

    violations = []
    full_text = "\n".join(lines)

    # Check if we're inside a data-transfer struct (exempt).
    current_struct: Optional[str] = None
    struct_depth = 0

    for i, line in enumerate(lines):
        stripped = line.strip()

        # Track struct/class scope.
        struct_match = re.match(r"\s*(?:struct|class)\s+(\w+)", line)
        if struct_match:
            current_struct = struct_match.group(1)
            struct_depth = 0

        # Track brace depth (rough — good enough for member detection).
        struct_depth += line.count('{') - line.count('}')
        if struct_depth <= 0:
            current_struct = None
            struct_depth = 0

        # Skip data-transfer structs.
        if current_struct in _DATA_TRANSFER_STRUCTS:
            continue

        # Skip comments.
        if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
            continue

        # Check hardcoded scene constants.
        for pat, msg in _HARDCODED_SCENE_PATTERNS:
            if pat.search(line):
                violations.append(Violation(
                    path, i + 1, "godot-native/hardcoded-scene-val", msg
                ))

        # Check parallel-state members.
        # Only flag lines that look like member declarations (have a type + trailing_;)
        if "_" in line and (";" in line or "=" in line):
            for pat, node_type, read_method in _GODOT_OWNED_MEMBERS:
                if pat.search(line):
                    var_name = pat.pattern.replace(r"\b", "")
                    violations.append(Violation(
                        path, i + 1, "godot-native/parallel-state-cpp",
                        f"Member '{var_name}' duplicates {node_type} state. "
                        f"Read from {node_type}.{read_method} instead.",
                        severity="warning"
                    ))

    return violations


# ══════════════════════════════════════════════════════════════════════
#  RULE REGISTRY
# ══════════════════════════════════════════════════════════════════════

# All check functions, grouped by family.  Each returns list[Violation].
RULE_CHECKS: dict[str, list] = {
    "header": [
        check_header_pragma_once,
        check_header_description,
    ],
    "tiger": [
        check_tiger_assertion_density,
    ],
    "gpu": [
        check_gpu_static_assert,
    ],
    "module": [
        check_module_boundary,
    ],
    "naming": [
        check_naming_class_pascal,
    ],
    "godot-native": [
        check_godot_native_cpp,
    ],
}


# ══════════════════════════════════════════════════════════════════════
#  FILE DISCOVERY
# ══════════════════════════════════════════════════════════════════════

def find_source_files(root: Path, explicit_paths: list[str] | None = None) -> list[Path]:
    """Find all lintable source files.

    Scans:
      - src/**/*.h, src/**/*.cpp  (C++ sources)
      - project/demos/**/*.gd     (GDScript demo files)

    Skips: gen/, thirdparty/, godot-cpp/, and other SKIP_DIRS.
    """
    if explicit_paths:
        return [Path(p) for p in explicit_paths if Path(p).exists()]

    files: list[Path] = []

    for directory in [SRC_DIR, DEMO_DIR]:
        dir_path = root / directory
        if not dir_path.exists():
            continue
        for p in sorted(dir_path.rglob("*")):
            if not p.is_file():
                continue
            if p.name in SKIP_FILES:
                continue
            if any(skip in p.parts for skip in SKIP_DIRS):
                continue
            if p.suffix in (".h", ".cpp", ".gd"):
                # Skip auto-generated files.
                if any(p.name.endswith(sfx) for sfx in SKIP_SUFFIXES):
                    continue
                files.append(p)

    return files


# ══════════════════════════════════════════════════════════════════════
#  LINT ENGINE
# ══════════════════════════════════════════════════════════════════════

def lint_file(
    path: Path,
    root: Path,
    rule_filter: Optional[str],
    file_suppressions: dict[str, set[str]],
) -> tuple[list[Violation], int]:
    """Run all applicable rules on a single file.

    Returns (violations, suppressed_count).
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except (OSError, UnicodeDecodeError):
        return [], 0

    lines = text.splitlines()
    rel_path = str(path.relative_to(root)).replace("\\", "/")

    # Collect inline suppressions (line → set of rules).
    inline_suppressions: dict[int, set[str]] = {}
    for i, line in enumerate(lines):
        m = _SUPPRESS_RE.search(line)
        if m:
            # Suppression applies to the current line AND the next line.
            rule = m.group(1)
            inline_suppressions.setdefault(i, set()).add(rule)
            inline_suppressions.setdefault(i + 1, set()).add(rule)

    # File-level suppressions from lint.conf.
    file_rules = file_suppressions.get(rel_path, set())

    # Run checks.
    all_violations: list[Violation] = []
    suppressed = 0

    for family, checks in RULE_CHECKS.items():
        if rule_filter and not family.startswith(rule_filter):
            continue
        for check_fn in checks:
            raw = check_fn(rel_path, lines)
            for v in raw:
                # Check suppression.
                line_idx = v.line - 1  # 0-based for inline_suppressions
                line_supp = inline_suppressions.get(line_idx, set())
                # Match exact rule or family prefix.
                if (v.rule in line_supp
                    or v.rule.split("/")[0] in line_supp
                    or v.rule in file_rules
                    or v.rule.split("/")[0] in file_rules):
                    suppressed += 1
                    continue
                all_violations.append(v)

    return all_violations, suppressed


def run_lint(
    root: Path,
    files: list[Path],
    rule_filter: Optional[str] = None,
    verbose: bool = False,
) -> LintStats:
    """Run the linter on all given files and return stats."""
    stats = LintStats()
    file_suppressions = _load_file_suppressions(root)

    for path in files:
        violations, suppressed = lint_file(path, root, rule_filter, file_suppressions)
        stats.files_checked += 1
        stats.suppressed += suppressed

        if violations:
            stats.violations.extend(violations)
        else:
            stats.files_passed += 1

        if verbose and not violations:
            rel = path.relative_to(root)
            print(f"  ✓ {rel}")

    return stats


# ══════════════════════════════════════════════════════════════════════
#  CLI
# ══════════════════════════════════════════════════════════════════════

def _color(text: str, code: str) -> str:
    """ANSI color wrapper (no-op if not a TTY)."""
    if not sys.stdout.isatty():
        return text
    return f"\033[{code}m{text}\033[0m"


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="lint.py",
        description="Project convention linter for MessyerRaytracer.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            RULE FAMILIES
              header/        File header conventions
              tiger/         Assertion density (≥2 per function)
              gpu/           GPU struct static_assert
              module/        Module dependency boundary
              naming/        PascalCase classes, snake_case functions
              godot-native/  Godot-Native Principle enforcement

            SUPPRESSION
              Inline:  // rt-lint: suppress <rule>
                       # rt-lint: suppress <rule>
              File:    tools/lint.conf [suppress] section

            EXAMPLES
              python tools/lint.py                        # all files, all rules
              python tools/lint.py --rule godot-native    # only Godot-Native rules
              python tools/lint.py --summary              # rule-by-rule counts
              python tools/lint.py src/core/ray.h         # specific file
        """),
    )
    parser.add_argument(
        "files", nargs="*", default=[],
        help="Specific files to lint (default: scan src/ and project/demos/)",
    )
    parser.add_argument(
        "--rule", "-r", type=str, default=None,
        help="Only run rules from this family (e.g. 'godot-native', 'header', 'tiger')",
    )
    parser.add_argument(
        "--summary", "-s", action="store_true",
        help="Show rule-by-rule summary instead of per-file details",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Show passing files too",
    )
    parser.add_argument(
        "--root", type=str, default=None,
        help="Project root directory (auto-detected if not specified)",
    )

    args = parser.parse_args()

    # Auto-detect project root.
    if args.root:
        root = Path(args.root).resolve()
    else:
        # Walk up from this script to find SConstruct.
        candidate = Path(__file__).resolve().parent.parent
        if (candidate / "SConstruct").exists():
            root = candidate
        else:
            root = Path.cwd()

    # Resolve explicit file paths relative to root.
    explicit = None
    if args.files:
        explicit = []
        for f in args.files:
            p = Path(f)
            if not p.is_absolute():
                p = root / p
            explicit.append(str(p.resolve()))

    # Find files.
    files = find_source_files(root, explicit)
    if not files:
        print(_color("No source files found.", "33"))
        return 2

    # Validate rule filter.
    if args.rule and args.rule not in RULE_CHECKS:
        valid = ", ".join(sorted(RULE_CHECKS.keys()))
        print(_color(f"Unknown rule family '{args.rule}'. Valid: {valid}", "31"))
        return 2

    # Run.
    print(f"Linting {len(files)} files", end="")
    if args.rule:
        print(f" (rule: {args.rule})", end="")
    print(f" ...")

    stats = run_lint(root, files, args.rule, args.verbose)

    # Output.
    if args.summary:
        _print_summary(stats)
    else:
        _print_details(stats, root)

    # Final status.
    print()
    if stats.violations:
        print(_color(
            f"✖ {len(stats.violations)} violation(s) in {stats.files_failed} file(s)"
            f" ({stats.files_passed}/{stats.files_checked} passed"
            f", {stats.suppressed} suppressed)",
            "31"
        ))
        return 1
    else:
        print(_color(
            f"✓ All {stats.files_checked} files pass"
            f" ({stats.suppressed} suppressed)",
            "32"
        ))
        return 0


def _print_details(stats: LintStats, root: Path) -> None:
    """Print per-file violation details."""
    # Group by file.
    by_file: dict[str, list[Violation]] = {}
    for v in stats.violations:
        by_file.setdefault(v.file, []).append(v)

    for filepath in sorted(by_file.keys()):
        violations = by_file[filepath]
        print(f"\n{_color(filepath, '1')}")
        for v in sorted(violations, key=lambda x: x.line):
            sev_color = "31" if v.severity == "error" else "33"
            print(f"  {v.line:4d}: {_color(v.severity.upper(), sev_color)}: "
                  f"{v.message}  [{_color(v.rule, '36')}]")


def _print_summary(stats: LintStats) -> None:
    """Print rule-by-rule summary table."""
    counts = stats.rule_counts()
    if not counts:
        return

    max_rule = max(len(r) for r in counts.keys())
    print(f"\n{'Rule':<{max_rule}}  Count")
    print(f"{'─' * max_rule}  ─────")
    for rule, count in counts.items():
        color = "31" if count > 0 else "32"
        print(f"{rule:<{max_rule}}  {_color(str(count), color)}")

    print(f"\n{'Total':<{max_rule}}  {_color(str(len(stats.violations)), '31')}")
    if stats.suppressed:
        print(f"{'Suppressed':<{max_rule}}  {stats.suppressed}")


if __name__ == "__main__":
    sys.exit(main())
