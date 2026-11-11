@echo off
REM Launch UFC 5 in KytyPS5 with the configuration that measured 5.0-5.4 fps
REM in-fight on 2026-09-11. Thin wrapper over run_ufc5.ps1 - see that file and
REM "Replicating the 5 fps configuration" in D:\PS5\ledger.md.
REM
REM   run_ufc5.bat                  the 5 fps configuration
REM   run_ufc5.bat -Tag myrun       log to KytyLog-PPSA03541-myrun.txt
REM   run_ufc5.bat -Baseline        optional wins OFF, for A/B
REM   run_ufc5.bat -Validate        add --shader-validation
REM   run_ufc5.bat -Verify          SRT evaluator equivalence check (slow)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_ufc5.ps1" %*
