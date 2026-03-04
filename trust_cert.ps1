# Export cert from CurrentUser\My
$cert = Get-ChildItem -Path 'Cert:\CurrentUser\My' -CodeSigningCert | Where-Object { $_.Thumbprint -eq '4E4DA3F0EF0DFAB7B30FACB0402190405D2FC95D' }

# Add to Trusted Root (LocalMachine - needs admin)
$store = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
$store.Open("ReadWrite")
$store.Add($cert)
$store.Close()

# Add to Trusted Publishers (LocalMachine)
$store2 = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", "LocalMachine")
$store2.Open("ReadWrite")
$store2.Add($cert)
$store2.Close()

Write-Host "Certificate added to Trusted Root and Trusted Publishers"

# Re-sign
Set-AuthenticodeSignature -FilePath 'C:\Users\Admin\Desktop\CAPTCHA\dist\imlang.exe' -Certificate $cert -HashAlgorithm SHA256
