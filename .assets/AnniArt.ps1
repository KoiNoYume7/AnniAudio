#Requires -Version 7.0

# ------- ASCII ART ------- #
function Show-AnniArt {
    [CmdletBinding()]
    param(
        [string]$Text = 'ANNI'
    )

    try { Clear-Host } catch {}

    $asciiLines = @(
        '                                                                                              ',
        '                                                                                              ',
        '                                                                                              ',
        ' :::   ::: :::    ::: ::::    ::::  :::::::::: :::    :::     :::     ::::    :::     :::     ',
        ' :+:   :+: :+:    :+: +:+:+: :+:+:+ :+:        :+:    :+:   :+: :+:   :+:+:   :+:   :+: :+:   ',
        '  +:+ +:+  +:+    +:+ +:+ +:+:+ +:+ +:+        +:+    +:+  +:+   +:+  :+:+:+  +:+  +:+   +:+  ',
        '   +#++:   +#+    +:+ +#+  +:+  +#+ +#++:++#   +#++:++#++ +#++:++#++: +#+ +:+ +#+ +#++:++#++: ',
        '    +#+    +#+    +#+ +#+       +#+ +#+        +#+    +#+ +#+     +#+ +#+  +#+#+# +#+     +#+ ',
        '    #+#    #+#    #+# #+#       #+# #+#        #+#    #+# #+#     #+# #+#   #+#+# #+#     #+# ',
        '    ###     ########  ###       ### ########## ###    ### ###     ### ###    #### ###     ### ',
        '                                                                                              ',
        '                                                                                              ',
        '                                                                                              '
    )

    $startColor = @{ R = 0; G = 255; B = 255 }   # Cyan
    $endColor = @{ R = 255; G = 0; B = 0 }       # Red

    function Get-InterpolatedColor {
        param([int]$pos, [int]$max, $start, $end)
        $r = [math]::Round($start.R + ($end.R - $start.R) * ($pos / $max))
        $g = [math]::Round($start.G + ($end.G - $start.G) * ($pos / $max))
        $b = [math]::Round($start.B + ($end.B - $start.B) * ($pos / $max))
        return @{ R = $r; G = $g; B = $b }
    }

    function Write-ColorChar {
        param([char]$char, [int]$r, [int]$g, [int]$b)
        $esc = "`e[38;2;${r};${g};${b}m"
        $reset = "`e[0m"
        Write-Host -NoNewline "${esc}${char}${reset}"
    }

    foreach ($line in $asciiLines) {
        $chars = $line.ToCharArray()
        $maxIndex = $chars.Length - 1
        for ($i = 0; $i -le $maxIndex; $i++) {
            $color = Get-InterpolatedColor -pos $i -max $maxIndex -start $startColor -end $endColor
            Write-ColorChar -char $chars[$i] -r $color.R -g $color.G -b $color.B
        }
        Write-Host ''
    }
}
# ------- END ASCII ART ------- #
