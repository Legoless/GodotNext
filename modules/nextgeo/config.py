def can_build(env, platform):
    # The offline DAG builder is built on meshoptimizer. Without it, nextgeo is disabled
    # and imports skip virtual geometry cleanly, like LOD generation.
    env.module_add_dependencies("nextgeo", ["meshoptimizer"])
    return not env["disable_3d"]


def configure(env):
    pass
