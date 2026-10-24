set /a errorno=1
for /F "delims=#" %%E in ('"prompt #$E# & for %%E in (1) do rem"') do set "esc=%%E"

@rem set "Configuration=Debug"
@rem set "Platform=Win32"

set "BIN=.\bin\!Platform!_!Configuration!"
set "TEST_FILES=..\..\tests\data\COPYING"

echo !BIN!\lz4 -h
     !BIN!\lz4 -h	|| goto :ERROR

echo !BIN!\lz4 -i1b
     !BIN!\lz4 -i1b	|| goto :ERROR

echo !BIN!\lz4 -i1b5
     !BIN!\lz4 -i1b5	|| goto :ERROR

echo !BIN!\lz4 -i1b10
     !BIN!\lz4 -i1b10	|| goto :ERROR

echo !BIN!\lz4 -i1b15
     !BIN!\lz4 -i1b15	|| goto :ERROR
     
@rem empty legacy stream : threadpool used to wait forever
type nul > tmp-empty
echo !BIN!\lz4 -l -f tmp-empty tmp-empty.lz4
     !BIN!\lz4 -l -f tmp-empty tmp-empty.lz4	|| goto :ERROR
echo !BIN!\lz4 -d -f tmp-empty.lz4 tmp-empty.out
     !BIN!\lz4 -d -f tmp-empty.lz4 tmp-empty.out	|| goto :ERROR
del tmp-empty tmp-empty.lz4 tmp-empty.out

@rem multithreaded compression (liblz4 worker threads) : reported by the version line (LZ4F_getMaxNbWorkers()),
@rem since output is the same whatever the nb of threads
echo !BIN!\lz4 -V ^| findstr multithread
     !BIN!\lz4 -V | findstr /c:"multithread"	|| goto :ERROR
echo !BIN!\datagen -g13M ^> tmp-mt
     !BIN!\datagen -g13M > tmp-mt	|| goto :ERROR
for %%O in ("-1" "-1 -BD" "-9 -BD -BX") do (
    echo !BIN!\lz4 %%~O -T1 / -T4 tmp-mt
    !BIN!\lz4 -q -f %%~O -T1 tmp-mt tmp-mt-T1.lz4	|| goto :ERROR
    !BIN!\lz4 -q -f %%~O -T4 tmp-mt tmp-mt-T4.lz4	|| goto :ERROR
    fc /b tmp-mt-T1.lz4 tmp-mt-T4.lz4 > nul	|| goto :ERROR
    !BIN!\lz4 -q -d -f tmp-mt-T4.lz4 tmp-mt.out	|| goto :ERROR
    fc /b tmp-mt tmp-mt.out > nul	|| goto :ERROR
)
del tmp-mt tmp-mt-T1.lz4 tmp-mt-T4.lz4 tmp-mt.out

echo fullbench
!BIN!\fullbench.exe --no-prompt -i1 %TEST_FILES%	|| goto :ERROR

echo fuzzer
!BIN!\fuzzer.exe -v -T30s				|| goto :ERROR


set /a errorno=0
goto :END

:ERROR

:END
exit /B %errorno%
