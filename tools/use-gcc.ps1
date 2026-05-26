$GccBin = Join-Path $PSScriptRoot "w64devkit\bin"

if (($env:Path -split ';') -notcontains $GccBin) {
    $env:Path = "$GccBin;$env:Path"
}

gcc.exe --version
