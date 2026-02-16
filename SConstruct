#!/usr/bin/env python
import os
import sys

from methods import print_error


libname = "Raytracer"
projectdir = "project"

localEnv = Environment(tools=["default"], PLATFORM="")

# Build profiles can be used to decrease compile times.
# You can either specify "disabled_classes", OR
# explicitly specify "enabled_classes" which disables all other classes.
# Modify the example file as needed and uncomment the line below or
# manually specify the build_profile parameter when running SCons.

# localEnv["build_profile"] = "build_profile.json"

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)
opts.Update(localEnv)

Help(opts.GenerateHelpText(localEnv))

env = localEnv.Clone()

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error("""godot-cpp is not available within this folder, as Git submodules haven't been initialized.
Run the following command to download godot-cpp:

    git submodule update --init --recursive""")
    sys.exit(1)

env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs})

env.Append(CPPPATH=["src/"])

# Enable C++ exception handling on MSVC (required for try/catch in ThreadPool).
if env.get("is_msvc", False):
    env.Append(CCFLAGS=["/EHsc"])

# ---------------------------------------------------------------------------
# Shader embedding: convert .glsl source to C++ header with raw string literal
# ---------------------------------------------------------------------------
def embed_shader_action(target, source, env):
    with open(str(source[0]), "r") as f:
        glsl = f.read()
    src_basename = os.path.basename(str(source[0]))
    # bvh_traverse.comp.glsl -> BVH_TRAVERSE_GLSL
    var_name = src_basename.split(".")[0].upper() + "_GLSL"
    with open(str(target[0]), "w") as f:
        f.write("#pragma once\n")
        f.write("// Auto-generated from {} -- DO NOT EDIT\n".format(src_basename))
        f.write('static const char *{} = R"(\n'.format(var_name))
        f.write(glsl)
        f.write(')";\n')
    return 0

shader_bvh = env.Command(
    "src/gpu/shaders/bvh_traverse.gen.h",
    "src/gpu/shaders/bvh_traverse.comp.glsl",
    embed_shader_action,
)

shader_rt_reflections = env.Command(
    "src/gpu/shaders/rt_reflections.gen.h",
    "src/gpu/shaders/rt_reflections.comp.glsl",
    embed_shader_action,
)

shader_rt_denoise_spatial = env.Command(
    "src/gpu/shaders/rt_denoise_spatial.gen.h",
    "src/gpu/shaders/rt_denoise_spatial.comp.glsl",
    embed_shader_action,
)

shader_rt_denoise_temporal = env.Command(
    "src/gpu/shaders/rt_denoise_temporal.gen.h",
    "src/gpu/shaders/rt_denoise_temporal.comp.glsl",
    embed_shader_action,
)

shader_rt_composite = env.Command(
    "src/gpu/shaders/rt_composite.gen.h",
    "src/gpu/shaders/rt_composite.comp.glsl",
    embed_shader_action,
)

shader_headers = [shader_bvh, shader_rt_reflections, shader_rt_denoise_spatial,
                  shader_rt_denoise_temporal, shader_rt_composite]

sources = Glob("src/godot/*.cpp") + Glob("src/gpu/*.cpp") + Glob("src/modules/*/*.cpp")
env.Depends(sources, shader_headers)

if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

# .dev doesn't inhibit compatibility, so we don't need to key it.
# .universal just means "compatible with all relevant arches" so we don't need to key it.
suffix = env['suffix'].replace(".dev", "").replace(".universal", "")

lib_filename = "{}{}{}{}".format(env.subst('$SHLIBPREFIX'), libname, suffix, env.subst('$SHLIBSUFFIX'))

library = env.SharedLibrary(
    "bin/{}/{}".format(env['platform'], lib_filename),
    source=sources,
)

copy = env.Install("{}/bin/{}/".format(projectdir, env["platform"]), library)

default_args = [library, copy]
Default(*default_args)
