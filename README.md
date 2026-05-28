
# Project Charter & Architectural Specification: AMIO
**Asynchronous Multidimensional Input Output Library**
## 1. Project Charge
### 1.1 Mandate
The AMIO project is charged with the design, development, and delivery of a production-grade, standalone, hardware-agnostic data I/O engine tailored for the NOAA NWS Office of Modeling and Development and the broader Earth system modeling ecosystem.
### 1.2 Core Problem Statement
Traditional Earth system model I/O frameworks bind scientific compute loops directly to specific, rigid file serialization APIs. As models migrate between on-premises supercomputers and cloud architectures, this coupling introduces severe performance bottlenecks. On-prem parallel file systems (Lustre/GPFS) require massive, contiguous block writes, while cloud object storage (S3/GCS) requires highly concurrent, chunked REST transactions. Furthermore, models require frequent, synchronized input ingestion (forcing fields, boundary conditions) and operational GRIB2 dissemination. Legacy pipelines force compromises or costly post-processing ETL (Extract, Transform, Load) tasks to satisfy these competing needs.
### 1.3 Mission Objective
AMIO will completely decouple scientific compute code from the underlying physical storage architecture through an adaptable high-level architecture plan. It must provide a frictionless, bidirectional, asynchronous data pipeline that safely ingests and outputs multidimensional memory views directly from/to application layers. It must route data dynamically across heterogeneous files, allowing for simultaneous handling of optimized on-prem NetCDF formats, cloud-native Zarr v3 datasets, or operational GRIB2 streams. AMIO must run inline with minimal CPU overhead, guaranteeing bit-for-bit scientific reproducibility while insulating host models from cloud network latencies and physical storage constraints.
## 2. High-Level Architecture Plan (Adaptable Design)
The defining characteristic of the AMIO architecture plan is its structural adaptability, resource isolation, and memory safety. By utilizing a highly decoupled, layered approach driven by software design patterns, the engine adapts to changing hardware (CPUs vs. GPUs), changing storage environments (On-Prem vs. Cloud), and changing data format mandates without requiring modifications to the host scientific model.
The architecture utilizes the **Facade Design Pattern** at the public boundary to isolate legacy code from modern C++ systems, an **Internal Staging Pool** to manage data race conditions for both reads and writes, and the **Strategy Design Pattern** internally via an object factory to swap storage formats seamlessly at runtime.
### 2.1 Structural Diagram
```text
       ┌────────────────────────────────────────────────────────┐
       │       Host Application (e.g., UFS / FV3 / JEDI)        │
       │   Fortran Compute Loops (Aggressive Memory Reuse)      │
       └────────────────────────────────────────────────────────┘
             ▲                                            │
             │ (Instant Pointer Pass)                     │ (Instant Snapshot Copy)
             │                                            ▼
+───────────────────────────────────────────────────────────────────────────────────+
|                    AMIO Public Frontend API (Flat C Interface)                    |
+───────────────────────────────────────────────────────────────────────────────────+
             ▲                                            │
             │                                            ▼
+───────────────────────────────────────────────────────────────────────────────────+
|               AMIO Internal Staging Pool & Pre-Fetch Queue (RAM)                  |
|   [Prefetched Step N+1 Memory]               [Staged Step N Output Snapshot Memory] |
+───────────────────────────────────────────────────────────────────────────────────+
             ▲                                            │
             │ (Async Background Read)                    │ (Async Background Write)
             ▼                                            ▼
+───────────────────────────────────────────────────────────────────────────────────+
|            AMIO Core Infrastructure Engine (C++ / ecKit / mdspan)                 |
|   - Thread & Resource Affinity Configuration (Core Pinning / Dedicated Ranks)      |
|   - Polymorphic Factory Mapping             - Dynamic Index Space Transformation  |
+───────────────────────────────────────────────────────────────────────────────────+
                                      │
         ┌────────────────────────────┼────────────────────────────┐
         ▼                            ▼                            ▼
+-----------------+          +-----------------+          +-----------------+
| NetCDF-4 Engine |          | Zarr v3 Engine  |          |  GRIB2 Engine   |
| (Parallel HDF5) |          |  (TensorStore)  |          | (nceplibs-g2c)  |
+-----------------+          +-----------------+          +-----------------+
         │                            │                            │
         ▼                            ▼                            ▼
+-----------------+          +-----------------+          +-----------------+
| Parallel FS     |          | Cloud S3/GCS or |          | Operational     |
| (Lustre/GPFS)   |          | Sharded Lustre  |          | Dissemination   |
+-----------------+          +-----------------+          +-----------------+

```
### 2.2 The Adaptable Strata
 * **The Interoperability Layer (Frontend):** Bridges the language barrier. Legacy Fortran codes or pure C frameworks pass raw data pointers, shape configurations, and memory-space flags across a compiled, flat C boundary.
 * **The Memory Staging Pool & Pre-Fetch Queue:** To protect legacy models that aggressively reuse arrays across timesteps, AMIO decouples execution memory.
   * *For Writes:* It performs an ultra-fast synchronous copy into an internal buffer so Fortran can immediately overwrite its arrays.
   * *For Reads:* It maintains a look-ahead queue, reading upcoming timesteps into RAM buffers before the model requests them.
 * **The Memory Abstractor (mdspan):** Instantly wraps the safe, internal staging buffers into N-dimensional mathematical views using standard C++ templates.
 * **The Infrastructure Scaffold (ecKit):** Functions as the internal operational nervous system. It handles runtime YAML/JSON configuration parsing, maps storage targets via object factories, isolates background worker threads from the application's core compute tasks, and enforces exact hardware bindings.
 * **The Polymorphic Storage Factory:** Evaluates runtime configuration parameters to spin up the appropriate storage backend. Because it uses an abstract backend class, adding new formats, adding custom input drivers, or modifying existing ones requires zero code modifications to the frontend or the host applications.
## 3. System Requirements Specification
### 3.1 Functional Requirements
#### 3.1.1 Format Support and Translation
 * **Req-FN-1.1:** AMIO shall natively support reading and writing multidimensional datasets adhering to the NetCDF-4 Classic/Enhanced data model.
 * **Req-FN-1.2:** AMIO shall natively support reading and writing data structures adhering strictly to the core Zarr v3 specification.
 * **Req-FN-1.3:** AMIO shall natively support encoding and writing gridded meteorological fields to the operational GRIB2 format.
 * **Req-FN-1.4 (Mixed-Format Translation):** AMIO shall act as an on-the-fly format translator. It must support mixed workflows where input data is ingested from one format (e.g., NetCDF-4) and output data is serialized to an entirely different format (e.g., Zarr v3) during the same execution loop without requiring external post-processing ETL scripts.
#### 3.1.2 API and Interoperability (The C-Bridge)
 * **Req-FN-2.1:** The public API must be exposed as a compiled flat C interface using extern "C", completely hiding all C++ class definitions, templates, and compiler name-mangling from the linker.
 * **Req-FN-2.2:** AMIO shall include native Fortran 2003 iso_c_binding wrapping modules.
 * **Req-FN-2.3:** Internal state tracking, file handles, and connection contexts must be maintained internally and exposed to the calling application exclusively through opaque void* handles.
#### 3.1.4 Configuration and Ingestion Architecture
 * **Req-FN-3.1:** AMIO shall utilize ecKit utility sub-components to parse runtime YAML or JSON configuration manifests.
 * **Req-FN-3.2:** The library must dynamically instantiate the requested storage backend (NetCDF vs. Zarr v3 vs. GRIB2) for both read and write pathways at runtime via an internal eckit::Factory.
 * **Req-FN-3.3:** The library shall implement an internal metadata mapping bridge utilizing ecKit configurations to map human-readable metadata strings to the strict WMO numerical code tables required for GRIB2 headers.
 * **Req-FN-3.4 (Spatial Subsetting / Index Transformations):** The read API must allow the host application to request specific multidimensional bounds (strides/slices), enabling AMIO to execute index-space transformations to read only the necessary byte-ranges from the storage layer without loading global datasets into host memory.
#### 3.1.5 Data Preservation and Lossless Compression
 * **Req-FN-4.1:** The data pipeline must enforce strict bit-for-bit reproducibility. Lossy data compression frameworks or precision truncation mechanisms are strictly prohibited.
 * **Req-FN-4.2:** The Zarr v3 backend must apply a multi-threaded Byte-Shuffle filter followed by optimized lossless compression layouts (specifically Blosc or Zstandard) before committing chunks.
 * **Req-FN-4.3:** The GRIB2 backend shall enforce lossless Data Representation Templates (such as Adaptive Entropy Coding [AEC] via libaec or Lossless JPEG2000) within the underlying encoder.
### 3.2 Non-Functional Requirements
#### 3.2.1 Memory Performance, Safety, and Efficiency
 * **Req-NF-1.1:** Upon write API invocation, AMIO shall perform a synchronous memory copy from the host application's pointer into an internal AMIO-owned staging buffer before returning control to the host, ensuring the application can safely overwrite its arrays without race conditions.
 * **Req-NF-1.2:** Upon read API invocation for frequent inputs, AMIO shall implement look-ahead prefetching, loading and decompressing the requested step into the internal staging pool in the background, executing an instantaneous pointer swap when the host requests the data.
 * **Req-NF-1.3:** Post-staging, data bounding shall be executed via non-owning memory layouts utilizing header-only std::mdspan mechanics to prevent further duplicate memory allocations.
 * **Req-NF-1.4:** AMIO must natively support input buffers residing in both CPU Host Space and GPU Device Space (VRAM), utilizing Kokkos::deep_copy for hardware-accelerated staging transfers.
 * **Req-NF-1.5:** When routing to GRIB2, AMIO must inspect the mdspan for contiguity. If the array is strided, AMIO must seamlessly pack the slice into a contiguous 1D array prior to nceplibs-g2c ingestion.
#### 3.2.2 Asynchronous Concurrency and Resource Allocation
 * **Req-NF-2.1:** The library must decouple I/O execution from application compute constraints via background worker threads.
 * **Req-NF-2.2:** Threading systems must be managed through ecKit primitives, isolating I/O work cycles to prevent core starvation of the model's primary OpenMP or MPI resources. Host applications must be configured to support MPI_THREAD_MULTIPLE if background MPI-IO is required.
 * **Req-NF-2.3 (Thread Affinity / Core Pinning):** AMIO shall expose configuration parameters to bind its background I/O thread pool to specific, designated CPU cores or NUMA domains using ecKit resource binding layers, ensuring compute and I/O hardware execution are strictly isolated.
 * **Req-NF-2.4 (Task Splitting / Dedicated Ranks):** AMIO shall support execution across dedicated, isolated I/O ranks by utilizing eckit::mpi abstractions to split parallel communication groups away from primary host compute communicators.
#### 3.2.3 Storage Architecture Compatibility
 * **Req-NF-3.1:** When targeting Cloud Storage, the Zarr backend must interface directly with cloud object interfaces (AWS S3/GCS) over HTTP REST protocols using Google TensorStore's native KvStore layer.
 * **Req-NF-3.2:** When targeting on-premises parallel file systems (Lustre/GPFS), the Zarr backend must activate Zarr v3 Sharding (zarr3_sharding_indexed).
 * **Req-NF-3.3:** The NetCDF backend shall interface with netCDF-cxx4 and Parallel HDF5, utilizing native MPI-IO collective operations.
#### 3.2.4 Robustness and Diagnostics
 * **Req-NF-4.1:** Faults occurring within asynchronous I/O background worker threads must be caught natively using ecKit exception handlers and trigger parallel stack-traces across corresponding MPI ranks.
## 4. Integration & Packaging Requirements
| Target Constraint | Requirement Standard |
|---|---|
| **Toolchain Restrictions** | The codebase must compile using pure standard C, C++, and Fortran compilers. External languages or runtimes outside of the standard C/C++/Fortran/Python ecosystem (e.g., Rust/Cargo) are strictly barred from the dependency tree. |
| **Header Exposure** | Public headers must remain pristine, containing only standard C types and header-only kokkos/mdspan structures. Heavy dependencies (ecKit, Google TensorStore, netCDF-cxx4, nceplibs-g2c) must remain completely private to the internal build. |
| **Build Configuration** | Build systems must be engineered via standard, target-based CMake paradigms. |
| **Deployment Standard** | AMIO and its complete multi-tiered dependency graph (including Google's Bazel build chain for TensorStore) must be officially packaged, maintained, and deployed utilizing the Spack package manager (spack-stack). |
## 5. Deployment Risks & Mitigation Strategies
| Identified Risk | Severity | Mitigation Strategy |
|---|---|---|
| **TensorStore Build Complexity (Bazel)**
Google TensorStore relies on the Bazel build system, which aggressively fetches external network dependencies at compile time. This poses a significant deployment risk in air-gapped, high-security NOAA/NWS HPC environments. | High | **Primary:** Work with NOAA spack-stack maintainers to pre-cache TensorStore Bazel dependencies for offline deployment.
**Fallback:** If Bazel cannot be supported by the target environment's administrators, AMIO's Zarr backend will be formally downgraded to utilize **NCZarr** (Unidata's Zarr implementation built natively into netCDF-c). This fallback guarantees pure CMake/C compilation at the cost of cutting-edge Zarr v3 Sharding performance. |
| **MPI Thread Contention**
Executing NetCDF MPI-IO writes from an ecKit background thread while the host model performs MPI Halo exchanges may trigger fatal segmentation faults in legacy environments. | Medium | Target models must be updated to initialize the runtime using MPI_Init_thread() requesting MPI_THREAD_MULTIPLE. Furthermore, AMIO will provide a strictly synchronous execution toggle (amio_wait) for legacy clusters lacking robust multi-threading support. |


## License

This project is part of NOAA-EMC Ecosystem.

See LICENSE and DISCLAIMER for details.
