# Injects the cached MSVC environment into the current PowerShell session.
$envText = (& "$PSScriptRoot\msvcenv.exe" print) -join [Environment]::NewLine
if ($envText) { Invoke-Expression $envText }
