import os
import sys

from methods import print_error


def is_enabled():
    # Disabled by default (plan §19). Enable with module_oidn_enabled=yes and either
    # oidn_backend=mock (CI/testing) or oidn_backend=oidn oidn_sdk_path=<path>.
    return False


def can_build(env, platform):
    return not env["disable_3d"] and env["rendering_device"] and platform in ("linuxbsd", "windows", "macos")


def get_opts(platform):
    from SCons.Variables import EnumVariable

    return [
        EnumVariable(
            "oidn_backend",
            "OpenImageDenoise backend for modules/oidn ('oidn' uses developer-supplied oidn_sdk_path; 'mock' builds without the SDK for CI/tests)",
            "oidn",
            ("oidn", "mock"),
            ignorecase=2,
        ),
        (
            "oidn_sdk_path",
            "Path to a developer-supplied Intel OpenImageDenoise SDK root outside the repository (contains include/OpenImageDenoise/oidn.h and lib/)",
            "",
        ),
    ]


def _find_oidn_library(sdk_path, platform):
    lib_candidates = {
        "windows": ["OpenImageDenoise.lib", "libOpenImageDenoise.dll.a", "libOpenImageDenoise.a"],
        "linuxbsd": ["libOpenImageDenoise.so", "libOpenImageDenoise.a"],
        "macos": ["libOpenImageDenoise.dylib", "libOpenImageDenoise.a"],
    }.get(platform, [])

    for subdir in ("lib", "lib64"):
        lib_dir = os.path.join(sdk_path, subdir)
        if not os.path.isdir(lib_dir):
            continue
        for candidate in lib_candidates:
            if os.path.isfile(os.path.join(lib_dir, candidate)):
                return lib_dir
        # Allow versioned shared libraries on Linux (e.g. libOpenImageDenoise.so.2).
        if platform == "linuxbsd":
            for entry in os.listdir(lib_dir):
                if entry.startswith("libOpenImageDenoise.so."):
                    return lib_dir
    return None


def configure(env):
    backend = env["oidn_backend"]
    if backend == "mock":
        env["oidn_resolved_include"] = ""
        env["oidn_resolved_libpath"] = ""
        return

    sdk_path = str(env["oidn_sdk_path"]).strip()
    if not sdk_path:
        print_error(
            "modules/oidn: oidn_backend=oidn requires a developer-supplied oidn_sdk_path=<path> "
            "(outside the repository, plan §19). Pass oidn_backend=mock for CI/testing without the SDK."
        )
        sys.exit(255)

    sdk_abs = os.path.realpath(os.path.expanduser(sdk_path))
    if not os.path.isdir(sdk_abs):
        print_error(f"modules/oidn: oidn_sdk_path directory does not exist: '{sdk_path}'.")
        sys.exit(255)

    repo_root = os.path.realpath(env.Dir("#").abspath)
    try:
        inside_repo = os.path.commonpath([repo_root, sdk_abs]) == repo_root
    except ValueError:
        inside_repo = False
    if inside_repo:
        print_error(f"modules/oidn: oidn_sdk_path '{sdk_abs}' must stay outside the repository (plan §19).")
        sys.exit(255)

    include_dir = os.path.join(sdk_abs, "include")
    header_path = os.path.join(include_dir, "OpenImageDenoise", "oidn.h")
    if not os.path.isfile(header_path):
        print_error(
            f"modules/oidn: missing OpenImageDenoise header at '{header_path}'. "
            "Check that oidn_sdk_path points to the root of an Intel OpenImageDenoise SDK installation."
        )
        sys.exit(255)

    lib_dir = _find_oidn_library(sdk_abs, env["platform"])
    if lib_dir is None:
        print_error(
            f"modules/oidn: could not find the OpenImageDenoise library under '{sdk_abs}/lib' or '{sdk_abs}/lib64' "
            f"for platform '{env['platform']}'."
        )
        sys.exit(255)

    env["oidn_resolved_include"] = include_dir
    env["oidn_resolved_libpath"] = lib_dir
