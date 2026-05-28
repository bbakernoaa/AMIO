# Configuration (Manifests)

AMIO is configured through YAML manifest files that specify the backend driver,
staging pool parameters, worker pool settings, and output paths.

## Manifest Structure

```yaml
amio:
  version: "0.1.0"

  staging:
    buffer_count: 8              # Number of staging buffers [1, 4096]
    buffer_size_bytes: 52428800  # Per-buffer capacity in bytes [1, 1 GiB]

  workers:
    thread_count: 2              # Worker threads [1, 256]
    pin_threads: false           # CPU/NUMA pinning

  backend:
    name: "netcdf4"              # One of: netcdf4, zarr3, grib2
    options:
      # Backend-specific options (see below)

  output:
    path: "output.nc"
    overwrite: true
```

## Staging Pool

The staging pool is a bounded set of pre-allocated buffers used for
synchronous data snapshots during writes.

| Field | Type | Range | Default | Description |
|-------|------|-------|---------|-------------|
| `buffer_count` | integer | [1, 4096] | 8 | Number of staging buffers |
| `buffer_size_bytes` | integer | [1, 1 GiB] | 50 MiB | Per-buffer capacity |

!!! tip "Sizing the Staging Pool"
    Set `buffer_size_bytes` to at least the size of your largest single
    variable write. Set `buffer_count` to at least 2× the number of
    concurrent writes you expect to overlap with computation.

## Worker Pool

| Field | Type | Range | Default | Description |
|-------|------|-------|---------|-------------|
| `thread_count` | integer | [1, 256] | 1 | Number of background I/O threads |
| `pin_threads` | boolean | — | false | Pin threads to CPU cores |

## Backend: NetCDF-4

```yaml
backend:
  name: "netcdf4"
  options:
    parallel_io: false           # Use MPI parallel I/O
    compression:
      algorithm: "zlib"          # Lossless codec
      level: 4                   # Compression level [1-9]
    chunking:
      auto: true                 # Auto-determine chunk sizes
```

### NetCDF-4 Options

| Field | Values | Description |
|-------|--------|-------------|
| `parallel_io` | `true` / `false` | Enable MPI-IO collective writes |
| `compression.algorithm` | `zlib`, `szip` | Lossless compression codec |
| `compression.level` | 1–9 | Compression effort |
| `chunking.auto` | `true` / `false` | Auto-compute chunk dimensions |

## Backend: Zarr v3

```yaml
backend:
  name: "zarr3"
  options:
    mode: "nczarr"               # "tensorstore" or "nczarr"
    compression:
      algorithm: "zstd"          # One of: blosc, zstd
      level: 3
    chunking:
      auto: true
```

### Zarr v3 Options

| Field | Values | Description |
|-------|--------|-------------|
| `mode` | `tensorstore`, `nczarr` | Backend implementation |
| `compression.algorithm` | `blosc`, `zstd` | Lossless codec |
| `compression.level` | 1–22 (zstd) | Compression effort |

!!! note "TensorStore vs NCZarr"
    TensorStore mode supports cloud storage (S3, GCS, HTTPS) and Zarr v3
    sharding. NCZarr fallback mode uses the system netCDF-c library and
    supports only local file output without sharding.

## Backend: GRIB2

```yaml
backend:
  name: "grib2"
  options:
    packing: "complex_second_order"
    discipline: 0                # WMO discipline code
    compression:
      algorithm: "jpeg2000"      # One of: jpeg2000, aec
      target_ratio: 20
```

### GRIB2 Options

| Field | Values | Description |
|-------|--------|-------------|
| `packing` | `complex_second_order` | Data representation template |
| `discipline` | 0–255 | WMO GRIB2 discipline code |
| `compression.algorithm` | `jpeg2000`, `aec` | Lossless DRT |

## Validation Rules

AMIO validates the manifest on `amio_init`. Invalid configurations return
`AMIO_ERR_MANIFEST_INVALID` with a diagnostic message naming the failing field.

| Field | Constraint |
|-------|-----------|
| `staging.buffer_count` | ∈ [1, 4096] |
| `staging.buffer_size_bytes` | ∈ [1, 1,073,741,824] |
| `workers.thread_count` | ∈ [1, 256] |
| `backend.name` | One of `netcdf4`, `zarr3`, `grib2` |
| Compression codec | Must be on the lossless allow-list |

## Example Manifests

Complete example manifests are provided in `examples/manifests/`:

- [`netcdf4_manifest.yaml`](https://github.com/NOAA-EMC/AMIO/blob/main/examples/manifests/netcdf4_manifest.yaml)
- [`zarr3_manifest.yaml`](https://github.com/NOAA-EMC/AMIO/blob/main/examples/manifests/zarr3_manifest.yaml)
- [`grib2_manifest.yaml`](https://github.com/NOAA-EMC/AMIO/blob/main/examples/manifests/grib2_manifest.yaml)
