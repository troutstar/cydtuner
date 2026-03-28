param([double]$hz = 440.0, [int]$cycles = 3)

$url   = "http://cydtuner-test.local"
$range = 60
$step  = 5
$delay = 300

function Set-Hz([double]$f) {
    $b = '{"hz":' + ([math]::Round($f, 4)) + '}'
    $p = @{Method="POST"; ContentType="application/json"; Body=$b}
    irm "$url/synth" @p | Out-Null
}

function Cents-To-Hz([double]$base, [double]$cents) {
    return $base * [math]::Pow(2.0, $cents / 1200.0)
}

Write-Host "Target: $hz Hz  Range: +/- $range cents  Step: $step cents @ ${delay}ms"
Write-Host ""
Write-Host "Flat  (negative cents) : wheel spins one direction"
Write-Host "Sharp (positive cents) : wheel spins opposite direction"
Write-Host "Zero cents             : wheel stationary"
Write-Host "Note label snaps at 65 cents (not 50) - hysteresis"
Write-Host ""

for ($cycle = 1; $cycle -le $cycles; $cycle++) {
    Write-Host "=== Cycle $cycle of $cycles ==="

    Set-Hz (Cents-To-Hz $hz -$range)
    Write-Host "  FLAT (-$range cents) - 2s"
    Start-Sleep -Seconds 2

    Write-Host "  Sweeping flat to sharp..."
    for ($c = -$range; $c -le $range; $c += $step) {
        Set-Hz (Cents-To-Hz $hz $c)
        $sign = if ($c -ge 0) {"+"} else {""}
        $tag  = if ([math]::Abs($c) -le 5) {"[IN TUNE]"} else {"         "}
        Write-Host ("`r    $sign$c cents $tag") -NoNewline
        Start-Sleep -Milliseconds $delay
    }
    Write-Host ""

    Write-Host "  SHARP (+$range cents) - 2s"
    Start-Sleep -Seconds 2

    Write-Host "  Sweeping sharp to flat..."
    for ($c = $range; $c -ge -$range; $c -= $step) {
        Set-Hz (Cents-To-Hz $hz $c)
        $sign = if ($c -ge 0) {"+"} else {""}
        $tag  = if ([math]::Abs($c) -le 5) {"[IN TUNE]"} else {"         "}
        Write-Host ("`r    $sign$c cents $tag") -NoNewline
        Start-Sleep -Milliseconds $delay
    }
    Write-Host ""

    Set-Hz $hz
    Write-Host "  IN TUNE (0 cents) - 3s"
    Start-Sleep -Seconds 3
    Write-Host ""
}

Set-Hz $hz
Write-Host "Done."
