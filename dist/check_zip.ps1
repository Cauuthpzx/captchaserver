Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead('C:\Users\Admin\Desktop\CAPTCHA\dist\imlang-setup.zip')
$totalKB = [math]::Round((Get-Item 'C:\Users\Admin\Desktop\CAPTCHA\dist\imlang-setup.zip').Length / 1KB, 1)
Write-Host "ZIP size: $totalKB KB"
Write-Host ""
foreach ($entry in $zip.Entries) {
    $sizeKB = [math]::Round($entry.Length / 1KB, 1)
    Write-Host ("  " + $entry.Name + " - " + $sizeKB + " KB")
}
$zip.Dispose()
