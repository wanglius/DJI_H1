"""Confirmed H1 wavelength grid; v01 does not encode a wavelength table."""

H1_WAVELENGTH_START_NM = 340
H1_WAVELENGTH_END_NM = 1050
H1_SAMPLE_COUNT = H1_WAVELENGTH_END_NM - H1_WAVELENGTH_START_NM + 1
_H1_WAVELENGTHS_NM = tuple(range(H1_WAVELENGTH_START_NM,
                                H1_WAVELENGTH_END_NM + 1))


def spectrum_wavelengths_nm(sample_count: int) -> tuple[int, ...] | None:
    """Return 340..1050 nm inclusive; never stretch unknown lengths to fit."""
    return _H1_WAVELENGTHS_NM if sample_count == H1_SAMPLE_COUNT else None
