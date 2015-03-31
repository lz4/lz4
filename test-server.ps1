# http://msdn.microsoft.com/en-us/library/system.net.httplistener.getcontext(v=vs.110).aspx
# http://poshcode.org/4073
# https://gist.github.com/wagnerandrade/5424431

$url = 'http://localhost:8000/'
$basePath = (Get-Location).Path

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add($url)

$commands = @{
    '/server/shutdown/now' = { $listener.Stop() }
    '/server/cls'          = { Clear-Host }
    '/server/hello/world'  = { return "Hello, world" }
}

$listener.Start()
Write-Host "Listening at $($listener.Prefixes) ..."
while($listener.IsListening) {
    $context = $listener.GetContext()
    $response = $context.Response
    $requestUrl = $context.Request.Url
    $bytes = $null

    Write-Host "> $requestUrl"
    try {
        $localPath = $requestUrl.LocalPath
        $cmd = $commands.Get_Item($localPath)
        if($cmd -ne $null) {
            $bytes = & $cmd
        } else {
            $p = $basePath + ($localPath -replace "/", "\")
            $bytes = [System.IO.File]::ReadAllBytes($p)
        }
    } catch [Exception] {
        $response.StatusCode = 404
        $response.ContentType = 'text/plain;charset=utf-8'
        $bytes  = "ERROR: $($response.StatusCode)`n"
        $bytes += "Type: $($_.Exception.GetType().FullName)`n"
        $bytes += "Desc: $($_.Exception.Message)`n"
    }

    if($bytes -ne $null) {
        if($bytes -is [string]) {
            $bytes = [System.Text.Encoding]::UTF8.GetBytes($bytes)
        }
        $output = $response.OutputStream
        $output.Write($bytes, 0, $bytes.length)
        $output.Close()
    }
    $response.Close()
    Write-Host "< $($response.StatusCode)"
}
$listener.Stop()
