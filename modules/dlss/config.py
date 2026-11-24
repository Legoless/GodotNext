import os
import sys

from methods import print_error, print_warning

# Pinned NVIDIA DLSS and Streamline SDK tags (recorded in docs/godotnext/dlss-license.md, #98, #208).
DLSS_SDK_TAG = "v310.9.16"
STREAMLINE_SDK_TAG = "v2.14.16"

# Production Streamline plugin DLLs for Windows D3D12 (U6, #208). sl_nvperf.* is explicitly excluded.
STREAMLINE_WINDOWS_DLLS = (
    "sl.interposer.dll",
    "sl.common.dll",
    "sl.dlss_g.dll",
    "sl.reflex.dll",
    "sl.pcl.dll",
    "nvngx_dlssg.dll",
)


def is_enabled():
    # Disabled by default (plan §19, D19). Official GodotNext binaries stay 100% MIT and DLSS-free.
    # Enable with module_dlss_enabled=yes and either dlss_backend=mock (CI/tests) or
    # dlss_backend=ngx dlss_sdk_path=<path outside repository>.
    return False


def can_build(env, platform):
    if platform not in ("windows", "linuxbsd"):
        return False
    if env["arch"] != "x86_64":
        return False
    if not env["rendering_device"]:
        return False
    return bool(env["vulkan"] or (platform == "windows" and env.get("d3d12", False)))


def get_opts(platform):
    from SCons.Variables import BoolVariable, EnumVariable, PathVariable

    return [
        EnumVariable(
            "dlss_backend",
            "DLSS backend for modules/dlss ('ngx' compiles against developer-supplied dlss_sdk_path; 'mock' needs no SDK for CI/tests)",
            "ngx",
            ("ngx", "mock"),
            ignorecase=2,
        ),
        PathVariable(
            "dlss_sdk_path",
            "Path to a developer-supplied NVIDIA DLSS SDK checkout outside the repository (defaults to $DLSS_SDK)",
            os.environ.get("DLSS_SDK", ""),
            PathVariable.PathAccept,
        ),
        BoolVariable(
            "dlss_dev_runtime",
            "Copy dev/ DLSS and Streamline runtime libraries instead of rel/ (local debugging only; never distribute or export)",
            False,
        ),
        PathVariable(
            "dlss_notices_path",
            "Path to the developer-supplied NVIDIA DLSS §9.5 notices file outside the repository (required for export, #97)",
            "",
            PathVariable.PathAccept,
        ),
        PathVariable(
            "streamline_sdk_path",
            "Path to a developer-supplied NVIDIA Streamline SDK checkout outside the repository (defaults to $STREAMLINE_SDK; Windows D3D12 only, #208/#209)",
            os.environ.get("STREAMLINE_SDK", ""),
            PathVariable.PathAccept,
        ),
    ]


def _is_inside_repo(repo_root, candidate_abs):
    try:
        return os.path.commonpath([repo_root, candidate_abs]) == repo_root
    except ValueError:
        return False


def _find_linux_runtime(runtime_dir, prefix):
    if not os.path.isdir(runtime_dir):
        return None
    matches = sorted(
        entry
        for entry in os.listdir(runtime_dir)
        if entry.startswith(prefix) and os.path.isfile(os.path.join(runtime_dir, entry))
    )
    return os.path.join(runtime_dir, matches[-1]) if matches else None


def configure(env):
    env.Append(CPPDEFINES=["DLSS_ENABLED"])
    env.add_module_version_string("dlss")

    env["dlss_resolved_sdk"] = ""
    env["dlss_resolved_include"] = ""
    env["dlss_resolved_lib"] = ""
    env["dlss_resolved_runtime_files"] = []
    env["dlss_resolved_license"] = ""
    env["dlss_resolved_notices"] = ""
    env["dlss_resolved_streamline_sdk"] = ""
    env["dlss_resolved_streamline_include"] = ""
    env["dlss_resolved_streamline_lib"] = ""
    env["dlss_resolved_interposer_name"] = ""

    backend = env["dlss_backend"]
    if backend == "mock":
        return

    platform = env["platform"]
    if platform == "windows":
        if env.get("use_mingw", False):
            print_error(
                "modules/dlss: dlss_backend=ngx is not supported with use_mingw=yes "
                "(NVIDIA DLSS SDK provides MSVC-only .lib files). Build with MSVC or clang-cl, or pass dlss_backend=mock."
            )
            sys.exit(255)
        if not env.msvc:
            print_error(
                "modules/dlss: dlss_backend=ngx on Windows requires an MSVC-compatible toolchain (MSVC or clang-cl). "
                "Pass dlss_backend=mock for non-MSVC CI builds."
            )
            sys.exit(255)
        if env.get("debug_crt", False):
            print_error(
                "modules/dlss: dlss_backend=ngx is incompatible with debug_crt=yes (/MDd). "
                "Build with debug_crt=no (release CRT /MD or /MT) or pass dlss_backend=mock."
            )
            sys.exit(255)

    sdk_path = str(env["dlss_sdk_path"]).strip()
    if not sdk_path:
        print_error(
            "modules/dlss: dlss_backend=ngx requires dlss_sdk_path=<path> (or the DLSS_SDK environment variable) "
            "pointing to a developer-supplied NVIDIA DLSS SDK checkout outside the repository (plan §19). "
            "Pass dlss_backend=mock to build and run CI tests without the SDK."
        )
        sys.exit(255)

    sdk_abs = os.path.realpath(os.path.expanduser(sdk_path))
    if not os.path.isdir(sdk_abs):
        print_error(f"modules/dlss: dlss_sdk_path directory does not exist: '{sdk_path}'.")
        sys.exit(255)

    repo_root = os.path.realpath(env.Dir("#").abspath)
    if _is_inside_repo(repo_root, sdk_abs):
        print_error(f"modules/dlss: dlss_sdk_path '{sdk_abs}' must stay outside the repository (plan §19).")
        sys.exit(255)

    include_dir = os.path.join(sdk_abs, "include")
    header_path = os.path.join(include_dir, "nvsdk_ngx.h")
    if not os.path.isfile(header_path):
        print_error(
            f"modules/dlss: missing NGX header '{header_path}'. "
            f"Check that dlss_sdk_path points to an NVIDIA/DLSS checkout (pinned tag {DLSS_SDK_TAG})."
        )
        sys.exit(255)

    variant = "dev" if env["dlss_dev_runtime"] else "rel"
    runtime_files = []

    if platform == "windows":
        lib_name = "nvsdk_ngx_s.lib" if env.get("use_static_cpp", False) else "nvsdk_ngx_d.lib"
        lib_path = os.path.join(sdk_abs, "lib", "Windows_x86_64", "x64", lib_name)
        if not os.path.isfile(lib_path):
            print_error(
                f"modules/dlss: missing Windows static library '{lib_path}' "
                f"(use_static_cpp={'yes' if env.get('use_static_cpp', False) else 'no'})."
            )
            sys.exit(255)
        rt_dir = os.path.join(sdk_abs, "lib", "Windows_x86_64", variant)
        sr_dll = os.path.join(rt_dir, "nvngx_dlss.dll")
        if not os.path.isfile(sr_dll):
            print_error(f"modules/dlss: missing Windows {variant} runtime library '{sr_dll}'.")
            sys.exit(255)
        runtime_files.append((sr_dll, variant))
        rr_dll = os.path.join(rt_dir, "nvngx_dlssd.dll")
        if os.path.isfile(rr_dll):
            runtime_files.append((rr_dll, variant))
    else:
        lib_dir = os.path.join(sdk_abs, "lib", "Linux_x86_64")
        lib_path = ""
        for candidate in ("libnvsdk_ngx.a", "libsdk_nvngx.a"):
            p = os.path.join(lib_dir, candidate)
            if os.path.isfile(p):
                lib_path = p
                break
        if not lib_path:
            print_error(
                f"modules/dlss: missing Linux static library (libnvsdk_ngx.a or libsdk_nvngx.a) under '{lib_dir}'."
            )
            sys.exit(255)
        rt_dir = os.path.join(lib_dir, variant)
        sr_so = _find_linux_runtime(rt_dir, "libnvidia-ngx-dlss.so.")
        if not sr_so:
            print_error(f"modules/dlss: missing Linux {variant} runtime library (libnvidia-ngx-dlss.so.*) under '{rt_dir}'.")
            sys.exit(255)
        runtime_files.append((sr_so, variant))
        rr_so = _find_linux_runtime(rt_dir, "libnvidia-ngx-dlssd.so.")
        if rr_so:
            runtime_files.append((rr_so, variant))

    license_path = os.path.join(sdk_abs, "LICENSE.txt")
    if os.path.isfile(license_path):
        env["dlss_resolved_license"] = license_path

    notices_path = str(env["dlss_notices_path"]).strip()
    if notices_path:
        notices_abs = os.path.realpath(os.path.expanduser(notices_path))
        if not os.path.isfile(notices_abs):
            print_error(f"modules/dlss: dlss_notices_path file does not exist: '{notices_path}'.")
            sys.exit(255)
        if _is_inside_repo(repo_root, notices_abs):
            print_error(f"modules/dlss: dlss_notices_path '{notices_abs}' must stay outside the repository (plan §19).")
            sys.exit(255)
        env["dlss_resolved_notices"] = notices_abs

    # Streamline SDK (Windows D3D12 only, #208/#209).
    sl_path = str(env["streamline_sdk_path"]).strip()
    if sl_path:
        if platform != "windows" or not env.get("d3d12", False):
            print_warning(
                "modules/dlss: streamline_sdk_path is ignored on non-D3D12 / non-Windows builds (Streamline is Windows D3D12 only, plan §21.5)."
            )
        else:
            sl_abs = os.path.realpath(os.path.expanduser(sl_path))
            if not os.path.isdir(sl_abs):
                print_error(f"modules/dlss: streamline_sdk_path directory does not exist: '{sl_path}'.")
                sys.exit(255)
            if _is_inside_repo(repo_root, sl_abs):
                print_error(f"modules/dlss: streamline_sdk_path '{sl_abs}' must stay outside the repository (plan §19).")
                sys.exit(255)
            sl_include = os.path.join(sl_abs, "include")
            if not os.path.isfile(os.path.join(sl_include, "sl.h")):
                print_error(
                    f"modules/dlss: missing Streamline header '{os.path.join(sl_include, 'sl.h')}'. "
                    f"Check that streamline_sdk_path points to an NVIDIA Streamline SDK checkout (pinned tag {STREAMLINE_SDK_TAG})."
                )
                sys.exit(255)
            sl_lib = os.path.join(sl_abs, "lib", "x64", "sl.interposer.lib")
            if not os.path.isfile(sl_lib):
                print_error(f"modules/dlss: missing Streamline import library '{sl_lib}'.")
                sys.exit(255)
            sl_rel_dir = os.path.join(sl_abs, "bin", "x64")
            sl_dev_dir = os.path.join(sl_rel_dir, "development")
            for dll_name in STREAMLINE_WINDOWS_DLLS:
                if "nvperf" in dll_name.lower():
                    print_error(f"modules/dlss: excluded Streamline library '{dll_name}' is forbidden (U6, #208).")
                    sys.exit(255)
                if env["dlss_dev_runtime"] and os.path.isfile(os.path.join(sl_dev_dir, dll_name)):
                    dll_path = os.path.join(sl_dev_dir, dll_name)
                    dll_variant = "dev"
                else:
                    dll_path = os.path.join(sl_rel_dir, dll_name)
                    dll_variant = "dev" if env["dlss_dev_runtime"] else "rel"
                if not os.path.isfile(dll_path):
                    print_error(f"modules/dlss: missing Streamline {variant} runtime library '{dll_path}'.")
                    sys.exit(255)
                runtime_files.append((dll_path, dll_variant))

            env["dlss_resolved_streamline_sdk"] = sl_abs
            env["dlss_resolved_streamline_include"] = sl_include
            env["dlss_resolved_streamline_lib"] = sl_lib
            env["dlss_resolved_interposer_name"] = STREAMLINE_WINDOWS_DLLS[0]

    for src_path, _v in runtime_files:
        if "nvperf" in os.path.basename(src_path).lower():
            print_error(f"modules/dlss: excluded runtime '{src_path}' must never be listed in the manifest (U6, #208).")
            sys.exit(255)

    env["dlss_resolved_sdk"] = sdk_abs
    env["dlss_resolved_include"] = include_dir
    env["dlss_resolved_lib"] = lib_path
    env["dlss_resolved_runtime_files"] = runtime_files
