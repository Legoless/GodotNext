"""Offline tools for the §13.4 metrics Q1 through Q21."""

from godotnext_metrics.counters import (
    q2_card_hit_coverage,
    q14_fallback_coverage,
    q15_geometry_memory,
    q16_clamped_energy,
    q18_rt_coverage,
)
from godotnext_metrics.image_metrics import q7_flip, q9_reflection_error, q10_ghost_trail
from godotnext_metrics.indirect import (
    q1_indirect_diffuse_error,
    q1_patch_error,
    q3_leak_ratio,
    q4a_step_convergence,
    q4b_continuous_convergence,
    q8_temporal_sigma,
)
from godotnext_metrics.pixels import q5_crack_pixels, q6_hole_pixels
from godotnext_metrics.q19_upscaler_flip import q19_upscaler_flip
from godotnext_metrics.q20_final_indirect import q20_final_indirect
from godotnext_metrics.q21_invalid_mv import q21_invalid_count, q21_invalid_motion_vectors
from godotnext_metrics.rays import q17_ray_query_parity
from godotnext_metrics.timing_metrics import q11_pass_time, q12_frame_time, q13_overhead

__all__ = [
    "q1_indirect_diffuse_error",
    "q1_patch_error",
    "q2_card_hit_coverage",
    "q3_leak_ratio",
    "q4a_step_convergence",
    "q4b_continuous_convergence",
    "q5_crack_pixels",
    "q6_hole_pixels",
    "q7_flip",
    "q8_temporal_sigma",
    "q9_reflection_error",
    "q10_ghost_trail",
    "q11_pass_time",
    "q12_frame_time",
    "q13_overhead",
    "q14_fallback_coverage",
    "q15_geometry_memory",
    "q16_clamped_energy",
    "q17_ray_query_parity",
    "q18_rt_coverage",
    "q19_upscaler_flip",
    "q20_final_indirect",
    "q21_invalid_count",
    "q21_invalid_motion_vectors",
]
