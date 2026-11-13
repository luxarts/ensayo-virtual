# Prueba de UDP entre dos PCs, sin Ensayo Virtual (cerrala antes: usa el mismo puerto).
#
#   Host:      powershell -ExecutionPolicy Bypass -File tools\udp-test.ps1 -Listen
#   Invitado:  powershell -ExecutionPolicy Bypass -File tools\udp-test.ps1 -Send <IP del host>
#
# El host escucha en el puerto 22140 y devuelve cada paquete que recibe; el invitado manda 5 y espera la respuesta.
param ([switch] $Listen, [string] $Send, [int] $Port = 22140, [int] $Seconds = 120)

$ErrorActionPreference = 'Stop'

if ($Listen)
{
    $udp = New-Object System.Net.Sockets.UdpClient $Port
    $udp.Client.ReceiveTimeout = 1000
    $deadline = (Get-Date).AddSeconds($Seconds)
    $count = 0
    Write-Host "Escuchando UDP $Port durante $Seconds s. Pedile al invitado que corra el script con -Send."

    try
    {
        while ((Get-Date) -lt $deadline)
        {
            try
            {
                $from = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Any, 0)
                $data = $udp.Receive([ref] $from)
                $count++
                Write-Host ("Recibido de {0}: '{1}' -> lo devuelvo" -f $from, [System.Text.Encoding]::UTF8.GetString($data)) -ForegroundColor Green
                [void] $udp.Send($data, $data.Length, $from)
            }
            catch [System.Net.Sockets.SocketException] { }   # timeout de 1 s: seguir escuchando
        }
    }
    finally { $udp.Close() }

    if ($count -eq 0)
    {
        Write-Host "No llegó ningún paquete: el UDP del invitado hacia esta PC está bloqueado (firewall/antivirus de su PC, o la red/Tailscale)." -ForegroundColor Red
    }
    exit 0
}

if ($Send)
{
    $udp = New-Object System.Net.Sockets.UdpClient
    $udp.Client.ReceiveTimeout = 2000
    $ok = 0

    try
    {
        for ($i = 1; $i -le 5; $i++)
        {
            $payload = [System.Text.Encoding]::UTF8.GetBytes("ensayo-udp-test $i")
            [void] $udp.Send($payload, $payload.Length, $Send, $Port)

            try
            {
                $from = New-Object System.Net.IPEndPoint ([System.Net.IPAddress]::Any, 0)
                $reply = $udp.Receive([ref] $from)
                $ok++
                Write-Host ("Respuesta {0} de {1}: '{2}'" -f $i, $from, [System.Text.Encoding]::UTF8.GetString($reply)) -ForegroundColor Green
            }
            catch [System.Net.Sockets.SocketException]
            {
                Write-Host "Paquete $i sin respuesta" -ForegroundColor Yellow
            }
        }
    }
    finally { $udp.Close() }

    if ($ok -gt 0)
    {
        Write-Host "UDP funciona en las dos direcciones con $Send`:$Port. Si la app sigue sin audio, el problema no es la red." -ForegroundColor Green
    }
    else
    {
        Write-Host "Sin respuesta. Si el host vio 'Recibido de ...', lo que se bloquea es la vuelta (firewall/antivirus de ESTA PC)." -ForegroundColor Red
        Write-Host "Si el host no vio nada, se bloquea la ida (firewall/antivirus de esta PC para UDP saliente, o la red)." -ForegroundColor Red
    }
    exit 0
}

Write-Host "Uso: -Listen (en el host)  |  -Send <IP del host> (en el invitado)"
exit 2
