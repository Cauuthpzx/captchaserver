$cert = Get-ChildItem -Path 'Cert:\CurrentUser\My' -CodeSigningCert | Where-Object { $_.Thumbprint -eq '4E4DA3F0EF0DFAB7B30FACB0402190405D2FC95D' }
Set-AuthenticodeSignature -FilePath 'C:\Users\Admin\Desktop\CAPTCHA\dist\imlang.exe' -Certificate $cert -HashAlgorithm SHA256
