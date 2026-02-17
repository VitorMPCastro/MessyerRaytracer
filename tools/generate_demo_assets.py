#!/usr/bin/env python3
"""generate_demo_assets.py — Reproducible procedural asset generator for demo scenes.

Generates all meshes, textures, and environment maps needed by demo scenes in
project/demos/. Run once to populate project/assets/. Generated files are
committed to the repo so cloning works without running this script.

Usage:
    python tools/generate_demo_assets.py

Output:
    project/assets/meshes/uv_sphere.obj          — 32×16 UV sphere with normals + UVs
    project/assets/meshes/subdivided_plane.obj    — 8×8 subdivided plane (for normal map)
    project/assets/meshes/room_box.obj            — Open-front box for lighting demo
    project/assets/textures/brick_normal.png      — 256×256 tiling brick normal map
    project/assets/textures/checker_albedo.png    — 256×256 checker pattern albedo
    project/assets/textures/flat_normal.png       — 8×8 flat normal (128, 128, 255)
    project/assets/environments/gradient_sky.hdr  — 512×256 gradient panorama (Radiance HDR)
"""

import math
import os
import struct
import zlib

# ============================================================
# Configuration
# ============================================================

ASSETS_ROOT = os.path.join(os.path.dirname(__file__), "..", "project", "assets")
MESHES_DIR = os.path.join(ASSETS_ROOT, "meshes")
TEXTURES_DIR = os.path.join(ASSETS_ROOT, "textures")
ENVS_DIR = os.path.join(ASSETS_ROOT, "environments")


def ensure_dirs():
    for d in [MESHES_DIR, TEXTURES_DIR, ENVS_DIR]:
        os.makedirs(d, exist_ok=True)


# ============================================================
# OBJ mesh generators
# ============================================================

def generate_uv_sphere(filepath, slices=32, stacks=16, radius=1.0):
    """Generate a UV sphere with normals and texture coordinates."""
    verts = []     # (x, y, z)
    normals = []   # (nx, ny, nz)
    uvs = []       # (u, v)
    faces = []     # list of (v/vt/vn, v/vt/vn, v/vt/vn)

    # Generate vertices.
    for j in range(stacks + 1):
        phi = math.pi * j / stacks
        for i in range(slices + 1):
            theta = 2.0 * math.pi * i / slices
            x = radius * math.sin(phi) * math.cos(theta)
            y = radius * math.cos(phi)
            z = radius * math.sin(phi) * math.sin(theta)
            nx, ny, nz = _normalize(x, y, z)
            u = i / slices
            v = j / stacks
            verts.append((x, y, z))
            normals.append((nx, ny, nz))
            uvs.append((u, v))

    # Generate faces (quads split into two triangles).
    for j in range(stacks):
        for i in range(slices):
            a = j * (slices + 1) + i + 1        # 1-indexed
            b = j * (slices + 1) + (i + 1) + 1
            c = (j + 1) * (slices + 1) + (i + 1) + 1
            d = (j + 1) * (slices + 1) + i + 1
            faces.append((a, b, c))
            faces.append((a, c, d))

    _write_obj(filepath, "UVSphere", verts, normals, uvs, faces)
    print(f"  Generated {filepath} ({len(verts)} verts, {len(faces)} tris)")


def generate_subdivided_plane(filepath, subdivisions=8, size=4.0):
    """Generate a subdivided XZ plane centered at origin, facing +Y."""
    verts = []
    normals = []
    uvs = []
    faces = []

    half = size / 2.0
    for j in range(subdivisions + 1):
        for i in range(subdivisions + 1):
            u = i / subdivisions
            v = j / subdivisions
            x = -half + u * size
            z = -half + v * size
            verts.append((x, 0.0, z))
            normals.append((0.0, 1.0, 0.0))
            uvs.append((u, v))

    for j in range(subdivisions):
        for i in range(subdivisions):
            a = j * (subdivisions + 1) + i + 1
            b = a + 1
            c = b + (subdivisions + 1)
            d = a + (subdivisions + 1)
            faces.append((a, b, c))
            faces.append((a, c, d))

    _write_obj(filepath, "SubdividedPlane", verts, normals, uvs, faces)
    print(f"  Generated {filepath} ({len(verts)} verts, {len(faces)} tris)")


def generate_room_box(filepath, width=8.0, height=4.0, depth=8.0):
    """Generate an open-front box (floor + 3 walls + ceiling), for lighting demo."""
    verts = []
    normals = []
    uvs = []
    faces = []
    idx = [0]  # mutable counter for 1-indexed faces

    def add_quad(v0, v1, v2, v3, n):
        """Add a quad (two triangles) with shared normal."""
        base = idx[0] + 1
        for v in [v0, v1, v2, v3]:
            verts.append(v)
            normals.append(n)
        uvs.extend([(0, 0), (1, 0), (1, 1), (0, 1)])
        faces.append((base, base + 1, base + 2))
        faces.append((base, base + 2, base + 3))
        idx[0] += 4

    hw, hh, hd = width / 2, height, depth / 2

    # Floor (Y=0, facing up).
    add_quad((-hw, 0, -hd), (hw, 0, -hd), (hw, 0, hd), (-hw, 0, hd), (0, 1, 0))
    # Ceiling (Y=height, facing down).
    add_quad((-hw, hh, hd), (hw, hh, hd), (hw, hh, -hd), (-hw, hh, -hd), (0, -1, 0))
    # Back wall (Z=-hd, facing +Z).
    add_quad((-hw, 0, -hd), (-hw, hh, -hd), (hw, hh, -hd), (hw, 0, -hd), (0, 0, 1))
    # Left wall (X=-hw, facing +X).
    add_quad((-hw, 0, hd), (-hw, hh, hd), (-hw, hh, -hd), (-hw, 0, -hd), (1, 0, 0))
    # Right wall (X=+hw, facing -X).
    add_quad((hw, 0, -hd), (hw, hh, -hd), (hw, hh, hd), (hw, 0, hd), (-1, 0, 0))

    _write_obj(filepath, "RoomBox", verts, normals, uvs, faces)
    print(f"  Generated {filepath} ({len(verts)} verts, {len(faces)} tris)")


def _normalize(x, y, z):
    length = math.sqrt(x * x + y * y + z * z)
    if length < 1e-10:
        return (0.0, 1.0, 0.0)
    return (x / length, y / length, z / length)


def _write_obj(filepath, obj_name, verts, normals, uvs, faces):
    """Write a Wavefront OBJ file with combined v/vn/vt indices (all same)."""
    with open(filepath, "w") as f:
        f.write(f"# {os.path.basename(filepath)} — Generated by tools/generate_demo_assets.py\n")
        f.write(f"o {obj_name}\n\n")
        for v in verts:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        f.write("\n")
        for vn in normals:
            f.write(f"vn {vn[0]:.6f} {vn[1]:.6f} {vn[2]:.6f}\n")
        f.write("\n")
        for vt in uvs:
            f.write(f"vt {vt[0]:.6f} {vt[1]:.6f}\n")
        f.write("\n")
        for tri in faces:
            a, b, c = tri
            f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")


# ============================================================
# Texture generators (raw PNG without PIL)
# ============================================================

def _write_png(filepath, width, height, pixels_rgb):
    """Write a minimal 8-bit RGB PNG from a flat list of (r, g, b) tuples.

    Uses Python's built-in struct and zlib — no PIL dependency.
    """
    def _chunk(chunk_type, data):
        c = chunk_type + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    # IHDR.
    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    # IDAT: each row starts with filter byte 0 (None).
    raw_rows = b""
    for y in range(height):
        raw_rows += b"\x00"  # filter: None
        for x in range(width):
            r, g, b = pixels_rgb[y * width + x]
            raw_rows += bytes([r, g, b])
    idat_data = zlib.compress(raw_rows, 9)

    with open(filepath, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")  # PNG signature
        f.write(_chunk(b"IHDR", ihdr_data))
        f.write(_chunk(b"IDAT", idat_data))
        f.write(_chunk(b"IEND", b""))


def generate_brick_normal_map(filepath, width=256, height=256):
    """Generate a tiling brick normal map.

    Bricks are laid in a standard running bond pattern. Mortar joints are
    recessed (normal points into the joint). Brick faces point straight up
    (128, 128, 255) in tangent space.
    """
    brick_w = width // 4    # 4 bricks per row
    brick_h = height // 8   # 8 rows
    mortar = 3              # mortar width in pixels
    bevel = 2               # edge bevel in pixels

    pixels = []
    for y in range(height):
        row_idx = y // brick_h
        offset = (brick_w // 2) if (row_idx % 2 == 1) else 0
        for x in range(width):
            bx = (x + offset) % brick_w
            by = y % brick_h

            # Check if in mortar.
            in_mortar = bx < mortar or by < mortar

            if in_mortar:
                # Mortar is slightly recessed — normal dips toward center.
                pixels.append((128, 128, 200))
            else:
                # Check distance to edge for bevel.
                dist_left = bx - mortar
                dist_bottom = by - mortar
                dist_right = brick_w - 1 - bx
                dist_top = brick_h - 1 - by
                min_dist = min(dist_left, dist_bottom, dist_right, dist_top)

                if min_dist < bevel:
                    # Compute bevel normal — push outward from edge.
                    nx, ny = 0.0, 0.0
                    if dist_left == min_dist:
                        nx = -0.5
                    elif dist_right == min_dist:
                        nx = 0.5
                    if dist_bottom == min_dist:
                        ny = -0.5
                    elif dist_top == min_dist:
                        ny = 0.5
                    # Encode tangent-space normal.
                    nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
                    r = int((nx * 0.5 + 0.5) * 255)
                    g = int((ny * 0.5 + 0.5) * 255)
                    b = int(nz * 255)
                    pixels.append((r, g, b))
                else:
                    # Flat brick face — tangent-space up.
                    pixels.append((128, 128, 255))

    _write_png(filepath, width, height, pixels)
    print(f"  Generated {filepath} ({width}×{height} brick normal map)")


def generate_checker_albedo(filepath, width=256, height=256, tile_size=32):
    """Generate a checker pattern albedo texture (light/dark gray)."""
    pixels = []
    for y in range(height):
        for x in range(width):
            cx = (x // tile_size) % 2
            cy = (y // tile_size) % 2
            if (cx + cy) % 2 == 0:
                pixels.append((200, 200, 200))
            else:
                pixels.append((80, 80, 80))

    _write_png(filepath, width, height, pixels)
    print(f"  Generated {filepath} ({width}×{height} checker albedo)")


def generate_flat_normal(filepath, width=8, height=8):
    """Generate a tiny flat normal map (128, 128, 255) — useful as default."""
    pixels = [(128, 128, 255)] * (width * height)
    _write_png(filepath, width, height, pixels)
    print(f"  Generated {filepath} ({width}×{height} flat normal)")


# ============================================================
# HDR environment map generator (Radiance RGBE format)
# ============================================================

def generate_gradient_sky_hdr(filepath, width=512, height=256):
    """Generate a simple sky gradient as a Radiance .hdr file.

    Zenith: deep blue (0.2, 0.3, 0.8)
    Horizon: warm white (0.8, 0.75, 0.7)
    Ground: dark brown (0.15, 0.12, 0.10)

    Uses Radiance RGBE encoding (Ward, 1991).
    """
    def _float_to_rgbe(r, g, b):
        """Convert linear RGB float to RGBE (8-bit per channel)."""
        v = max(r, g, b)
        if v < 1e-32:
            return (0, 0, 0, 0)
        # frexp returns (mantissa, exponent) where mantissa is in [0.5, 1.0).
        mantissa, exp = math.frexp(v)
        scale = mantissa * 256.0 / v
        return (
            min(255, int(r * scale)),
            min(255, int(g * scale)),
            min(255, int(b * scale)),
            exp + 128
        )

    # Zenith/horizon/ground colors (linear).
    zenith = (0.25, 0.35, 0.85)
    horizon = (0.85, 0.80, 0.75)
    ground = (0.15, 0.12, 0.10)

    with open(filepath, "wb") as f:
        # Header.
        f.write(b"#?RADIANCE\n")
        f.write(b"FORMAT=32-bit_rle_rgbe\n")
        f.write(b"\n")
        f.write(f"-Y {height} +X {width}\n".encode("ascii"))

        # Pixel data (uncompressed scanlines).
        for y in range(height):
            # v maps from 0 (top = zenith) to 1 (bottom = nadir).
            v = y / (height - 1)
            # Elevation angle: 0 = zenith, 0.5 = horizon, 1 = nadir.
            if v <= 0.5:
                # Sky: zenith to horizon.
                t = v / 0.5  # 0..1
                # Smooth interpolation.
                t = t * t * (3.0 - 2.0 * t)
                r = zenith[0] + (horizon[0] - zenith[0]) * t
                g = zenith[1] + (horizon[1] - zenith[1]) * t
                b = zenith[2] + (horizon[2] - zenith[2]) * t
            else:
                # Ground: horizon to ground.
                t = (v - 0.5) / 0.5
                t = t * t * (3.0 - 2.0 * t)
                r = horizon[0] + (ground[0] - horizon[0]) * t
                g = horizon[1] + (ground[1] - horizon[1]) * t
                b = horizon[2] + (ground[2] - horizon[2]) * t

            for x in range(width):
                rgbe = _float_to_rgbe(r, g, b)
                f.write(bytes(rgbe))

    print(f"  Generated {filepath} ({width}×{height} gradient sky HDR)")


# ============================================================
# Main
# ============================================================

def main():
    ensure_dirs()
    print("Generating demo assets...")
    print()

    # Meshes.
    print("[Meshes]")
    generate_uv_sphere(os.path.join(MESHES_DIR, "uv_sphere.obj"))
    generate_subdivided_plane(os.path.join(MESHES_DIR, "subdivided_plane.obj"))
    generate_room_box(os.path.join(MESHES_DIR, "room_box.obj"))
    print()

    # Textures.
    print("[Textures]")
    generate_brick_normal_map(os.path.join(TEXTURES_DIR, "brick_normal.png"))
    generate_checker_albedo(os.path.join(TEXTURES_DIR, "checker_albedo.png"))
    generate_flat_normal(os.path.join(TEXTURES_DIR, "flat_normal.png"))
    print()

    # Environments.
    print("[Environments]")
    generate_gradient_sky_hdr(os.path.join(ENVS_DIR, "gradient_sky.hdr"))
    print()

    print("Done! All assets written to project/assets/")


if __name__ == "__main__":
    main()
