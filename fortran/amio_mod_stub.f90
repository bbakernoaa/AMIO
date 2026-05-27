!------------------------------------------------------------------
! amio_mod_stub.f90
!
! Placeholder Fortran module for the `amio_fortran` STATIC archive
! established in task 1.1.  The full iso_c_binding interface block
! (bind(C, name="amio_*") declarations for every AMIO_C_API entry
! point and the assumed-shape-array convenience subroutines) arrives
! in tasks 11.1 and 11.2.
!
! Per the design, this module SHALL use only iso_c_binding kinds
! (c_int, c_int32_t, c_int64_t, c_size_t, c_ptr, c_char, c_bool,
! c_float, c_double) so that the Fortran wrapper depends only on
! `amio_public_headers` and on libamio.so's `extern "C"` symbols.
!------------------------------------------------------------------
module amio_mod
    use, intrinsic :: iso_c_binding, only : c_ptr
    implicit none
    private

    ! The public surface is intentionally empty during task 1.1 - the
    ! interface block is added in task 11.1.  Re-exporting c_ptr here
    ! anchors the module so the Fortran compiler emits a usable .mod
    ! file for the `amio_fortran` build target.
    public :: amio_core_handle_t

    type, bind(C) :: amio_core_handle_t
        type(c_ptr) :: opaque
    end type amio_core_handle_t

end module amio_mod
