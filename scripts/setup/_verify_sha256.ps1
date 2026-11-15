# Shared SHA256 verifier for setup-script downloads — supply-chain hardening (#3665).
# Dot-source this, then: Confirm-Sha256 -Path <file> -Expected <hex>
# Aborts the script (exit 1) on mismatch, so a tampered/MITM'd download never
# reaches the extract/build step.

function Confirm-Sha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Expected
    )
    if (-not (Test-Path $Path)) {
        Write-Error "Confirm-Sha256: file not found: $Path"
        exit 1
    }
    $actual = (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $Expected.ToLower()) {
        Write-Error "SHA256 mismatch for $Path`n  expected: $Expected`n  actual:   $actual"
        exit 1
    }
    Write-Host "Verified SHA256 of $(Split-Path $Path -Leaf)" -ForegroundColor Green
}
