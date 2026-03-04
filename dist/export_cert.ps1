$cert = Get-ChildItem -Path 'Cert:\CurrentUser\My' -CodeSigningCert | Where-Object { $_.Thumbprint -eq '4E4DA3F0EF0DFAB7B30FACB0402190405D2FC95D' }
if ($cert) {
    Export-Certificate -Cert $cert -FilePath "$PSScriptRoot\imlang.cer" -Type CERT
    Write-Host "Certificate exported: $($cert.Subject)"
} else {
    Write-Host "Certificate not found"
}
