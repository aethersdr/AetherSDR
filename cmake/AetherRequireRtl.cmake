# Release builds must not silently lose hardware support. Developer OFF is valid.
function(aether_require_rtl)
    if(REQUIRE_RTL AND (NOT ENABLE_RTL OR NOT RTLSDR_FOUND OR NOT RTL_FFTW3F_FOUND))
        message(FATAL_ERROR "REQUIRE_RTL needs ENABLE_RTL=ON, librtlsdr and float FFTW (fftw3f). Run the platform dependency setup; use REQUIRE_RTL=OFF only for intentional developer builds.")
    endif()
endfunction()
