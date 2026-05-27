// zarr_driver_stub.cpp
//
// Placeholder translation unit for the `driver_zarr` STATIC archive
// established in task 1.1.  The concrete Zarr_Driver implementation
// arrives in tasks 7.2 (TensorStore mode) and 7.3 (NCZarr fallback
// mode).  The compile-time selection between modes is gated by the
// AMIO_HAS_TENSORSTORE / AMIO_NCZARR_FALLBACK preprocessor flags set
// in the top-level CMakeLists.txt.

namespace amio::drivers::zarr {
[[maybe_unused]] static const int driver_zarr_scaffold_anchor = 0;
}  // namespace amio::drivers::zarr
