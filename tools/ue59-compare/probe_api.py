import unreal, os


def _tools_dir():
    """The gitignored data dir: GODOTNEXT_TOOLS_DIR, else <repo>/local/godotnext-tools."""
    tools_dir = os.environ.get("GODOTNEXT_TOOLS_DIR")
    if tools_dir:
        return tools_dir
    root = os.environ.get("GODOTNEXT_ROOT")
    if not root:
        try:
            root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
        except NameError:
            raise RuntimeError("Set GODOTNEXT_TOOLS_DIR or GODOTNEXT_ROOT: this interpreter did not set __file__.")
    return os.path.join(root, "local", "godotnext-tools")


TOOLS_DIR = _tools_dir()

EXPORTS = os.path.join(TOOLS_DIR, "ue59-compare", "exports")

def run():
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    # 1. Import axes.glb
    for src, dest in [
        (os.path.join(EXPORTS, 'swrt_kitbash/axes.glb'), '/Game/Ports/_Calib'),
        (os.path.join(EXPORTS, 'character_walk/skinned/s0000.glb'), '/Game/Ports/_ProbeSkel'),
        (os.path.join(EXPORTS, 'swrt_kitbash/sky.hdr'), '/Game/Ports/_ProbeSky'),
    ]:
        task = unreal.AssetImportTask()
        task.filename = src
        task.destination_path = dest
        task.automated = True
        task.replace_existing = True
        task.save = True
        asset_tools.import_asset_tasks([task])
        print('IMPORTED', dest, 'paths=', list(task.imported_object_paths))

    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    for p in ['/Game/Ports/_Calib', '/Game/Ports/_ProbeSkel', '/Game/Ports/_ProbeSky']:
        assets = reg.get_assets_by_path(p, recursive=True)
        print('ASSETS_IN', p, [(str(a.asset_name), str(a.asset_class_path.asset_name), str(a.package_name)) for a in assets])
        for a in assets:
            obj = a.get_asset()
            if isinstance(obj, unreal.StaticMesh):
                b = obj.get_bounding_box()
                c = (b.min + b.max) * 0.5
                print('  SM', a.asset_name, 'center=', (c.x, c.y, c.z), 'ext=', (b.max.x-b.min.x, b.max.y-b.min.y, b.max.z-b.min.z))
                ns = obj.get_editor_property('nanite_settings')
                print('    nanite_settings attrs=', [x for x in dir(ns) if 'enable' in x.lower() or 'preserve' in x.lower()][:10])
            elif isinstance(obj, unreal.SkeletalMesh):
                print('  SKM', a.asset_name, 'has_nanite_settings=', hasattr(obj, 'nanite_settings'))
                try:
                    ns = obj.get_editor_property('nanite_settings')
                    print('    skm nanite enabled=', ns.enabled)
                except Exception as e:
                    print('    skm nanite err=', e)
            elif isinstance(obj, unreal.AnimSequence):
                print('  ANIM', a.asset_name, 'len=', obj.get_play_length())
            elif isinstance(obj, unreal.TextureCube) or isinstance(obj, unreal.Texture2D):
                print('  TEX', type(obj).__name__, a.asset_name)

run()
