$url = "http://cydtuner-test.local"
$notes = @(
    @{name="E2"; hz=82.41},  @{name="A2"; hz=110.00}, @{name="D3"; hz=146.83},
    @{name="G3"; hz=196.00}, @{name="B3"; hz=246.94}, @{name="E4"; hz=329.63},
    @{name="F2"; hz=87.31},  @{name="C3"; hz=130.81}, @{name="F3"; hz=174.61},
    @{name="A3"; hz=220.00}, @{name="C4"; hz=261.63}, @{name="D4"; hz=293.66}
)
foreach ($n in $notes) {
    # Set new frequency
    $b = '{"hz":' + $n.hz + '}'
    $p = @{Method="POST"; ContentType="application/json"; Body=$b}
    irm "$url/synth" @p | Out-Null

    # Wait for transition frame to clear, then reset history
    Start-Sleep -Milliseconds 500
    irm "$url/history/clear" -Method POST | Out-Null

    # Accumulate clean frames
    Start-Sleep -Seconds 4

    $s = irm "$url/stats"
    $ok = [math]::Abs($s.cents_error.std) -lt 0.5
    $r = if ($ok) {"PASS"} else {"FAIL"}
    $mean = [double]$s.cents_error.mean
    $std  = [double]$s.cents_error.std
    Write-Host ("{0,-4} {1,7:F2}Hz  mean={2,7:F3}c  std={3:F3}c  n={4}  {5}" -f `
        $n.name, $n.hz, $mean, $std, $s.n_with_gt, $r)
}
