extends Node

## Prints the main RenderingDevice's async compute frame counters when the run ends (#174).
##
## run_async_compute_check.sh adds this script as an autoload through override.cfg for its scene
## captures. The line reads
##   GODOTNEXT_ASYNC_COMPUTE_REPORT {"submitted":<frames>,"fell_back":<frames>}
## where submitted counts frames that forked work to the async compute queue and joined it, and
## fell_back counts frames whose async lists had to run on the main queue.


func _exit_tree() -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		print("GODOTNEXT_ASYNC_COMPUTE_REPORT {\"rendering_device\":false}")
		return
	var report := rd.get_perf_report()
	var at := report.find(" async:")
	if at < 0:
		print("GODOTNEXT_ASYNC_COMPUTE_REPORT {\"rendering_device\":true,\"counter\":false}")
		return
	var parts := report.substr(at + 7).get_slice(" ", 0).split("/")
	print("GODOTNEXT_ASYNC_COMPUTE_REPORT " + JSON.stringify({ "submitted": int(parts[0]), "fell_back": int(parts[1]) }))
