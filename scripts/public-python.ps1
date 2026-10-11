function Resolve-PublicPython {
    $project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $workspace=Split-Path $project -Parent
    foreach($candidate in @($env:XIAOYUN_PYTHON,(Join-Path $workspace '.venv/Scripts/python.exe'),(Join-Path $project '.venv/Scripts/python.exe'),'D:\conda\envs_dirs\codex\Scripts\python.exe')) {
        if($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)){return $candidate}
    }
    throw 'Set XIAOYUN_PYTHON to the environment with all three project dependencies installed.'
}
