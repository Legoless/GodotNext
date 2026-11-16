extends RefCounted
## D3D12 GPU-based validation and the results checks cannot trust under it (#277).
##
## With rendering/rendering_device/d3d12/gpu_based_validation on (it takes effect together with
## --gpu-validation), storage-buffer and image atomics whose return value the shader does not use
## are lost on D3D12: atomicAdd() on a counter adds nothing, and nothing reports it. The Mesa DXIL
## compiler declares its atomic operations readnone (free of side effects), and once GPU-based
## validation patches a shader, such a call whose result nothing reads no longer runs. Atomics
## whose result is used, shared-memory atomics and plain stores are not affected.
##
## That holds for builds that link the upstream Mesa NIR library or GodotNext's 25.3.1-gn2. The
## GodotNext Mesa NIR library 25.3.1-gn3 and later declare the atomics as DXC does, and they land
## (#279); such a build prints "D3D12: GPU-based validation keeps atomics whose result is unused"
## after the line that turns GPU-based validation on. The runner then leaves out its not-checked
## note and fails the run when a result still reports NOT_CHECKED. Scripts cannot see which
## library the build links.
##
## A check whose result depends on such atomics reports it as NOT_CHECKED, with REASON, when this
## run asked for GPU-based validation and the result is what the limit produces. Anything else it
## still checks, so with 25.3.1-gn3 such a check passes as usual. A GPU-based validation run still
## counts its validation messages.

const SETTING := "rendering/rendering_device/d3d12/gpu_based_validation"
const NOT_CHECKED := "not_checked"
const REASON := "D3D12 GPU-based validation loses atomics whose result is unused (#277)"


## Whether this run asked for D3D12 GPU-based validation: the D3D12 driver with the setting on.
## Scripts cannot see --gpu-validation; without it the setting has no effect and the atomics land,
## so a check that finds its atomics landed passes either way.
static func requested() -> bool:
	return RenderingServer.get_current_rendering_driver_name() == "d3d12" and bool(ProjectSettings.get_setting(SETTING, false))
