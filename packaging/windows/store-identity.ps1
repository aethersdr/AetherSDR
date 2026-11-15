<#
.SYNOPSIS
    Set the Partner Center package identity for AetherSDR Store MSIX builds.

.DESCRIPTION
    These values are assigned on the Partner Center Product identity page after
    reserving the Microsoft Store product name. They are public package
    identity values, not signing secrets. Existing environment variables win so
    CI/release callers can still override them without editing this file.
#>

if ([string]::IsNullOrWhiteSpace($env:AETHERSDR_MSIX_IDENTITY_NAME)) {
    $env:AETHERSDR_MSIX_IDENTITY_NAME = "AetherSDR.AetherSDR"
}

if ([string]::IsNullOrWhiteSpace($env:AETHERSDR_MSIX_PUBLISHER)) {
    $env:AETHERSDR_MSIX_PUBLISHER = "CN=E03F94A2-AEAB-46D2-8BF1-6419C305CC44"
}

if ([string]::IsNullOrWhiteSpace($env:AETHERSDR_MSIX_DISPLAY_NAME)) {
    $env:AETHERSDR_MSIX_DISPLAY_NAME = "AetherSDR"
}

if ([string]::IsNullOrWhiteSpace($env:AETHERSDR_MSIX_PUBLISHER_DISPLAY_NAME)) {
    $env:AETHERSDR_MSIX_PUBLISHER_DISPLAY_NAME = "AetherSDR"
}
