function Resolve-NativeBuildOutput {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ProjectRoot,
        [Parameter(Mandatory)][string]$Variant,
        [string]$OutputDirectory
    )

    $root = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\', '/')
    $devRoot = [IO.Path]::GetFullPath((Join-Path $root 'build/native/dev')).TrimEnd('\', '/')
    $devPrefix = $devRoot + [IO.Path]::DirectorySeparatorChar
    if ($OutputDirectory) {
        if ([IO.Path]::IsPathRooted($OutputDirectory)) {
            throw 'OutputDirectory must be project-relative and inside build/native/dev.'
        }
        $output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
    } else {
        $output = [IO.Path]::GetFullPath((Join-Path $devRoot $Variant))
    }
    if (-not $output.StartsWith($devPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Native build outputs must stay inside build/native/dev.'
    }

    $cursor = $output
    while ($cursor.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $cursor.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band
                [IO.FileAttributes]::ReparsePoint)) {
            throw "Native build output cannot pass through a junction or symbolic link: $cursor"
        }
        if ($cursor.Equals($root, [StringComparison]::OrdinalIgnoreCase)) { break }
        $cursor = Split-Path -Parent $cursor
    }

    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $resolvedOutput = (Resolve-Path -LiteralPath $output).Path
    if (-not $resolvedOutput.StartsWith($devPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Resolved native build output escaped build/native/dev.'
    }
    return $output
}
