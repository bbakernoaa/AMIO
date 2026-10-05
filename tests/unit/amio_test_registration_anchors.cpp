// Test executables compile BackendFactory with mock or selected real drivers.
// These no-op force-link anchors are never compiled into libamio. Tests that
// compile a real driver use that driver's actual registration function.
#ifndef AMIO_TEST_REAL_NETCDF
extern "C" void amio_register_netcdf_driver() {}
#endif
#ifndef AMIO_TEST_REAL_ZARR
extern "C" void amio_register_zarr_driver() {}
#endif
#ifndef AMIO_TEST_REAL_GRIB2
extern "C" void amio_register_grib2_driver() {}
#endif
