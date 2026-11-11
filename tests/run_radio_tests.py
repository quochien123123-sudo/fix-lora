#!/usr/bin/env python3
"""Native HAL-mocked regression suite. No STM32/radio hardware is emulated.

Run against a driver with matching Core/Inc headers:
  python tests/run_radio_tests.py --driver /path/to/Core/Src/lora.c
To demonstrate the known original failure add --expect-delayed-failure.
"""
import argparse
import pathlib
import subprocess
import tempfile

tests=pathlib.Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--driver',type=pathlib.Path,default=tests.parent/'firmware-overrides/Master/Core/Src/lora.c')
p.add_argument('--expect-delayed-failure',action='store_true')
a=p.parse_args()
driver=a.driver.resolve()
tests=pathlib.Path(__file__).resolve().parent
cases=['init','version_zero','version_ff','rx','crc_error','crc_missing','tx_done',
       'delayed_tx_done','tx_timeout','spi_fault','pending_rx_blocks_tx',
       'receiving_blocks_tx','brownout','readback_fault','init_spi_fault','wrap_timeout']
failed=[]
with tempfile.TemporaryDirectory(prefix='lora-tests-') as d:
    binary=pathlib.Path(d)/'test_radio'
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-O0','-g',
                    '-I'+str(tests),'-I'+str(driver.parent.parent/'Inc'),
                    *(['-DTEST_DIAGNOSTICS'] if 'LoRaDiagnostics' in (driver.parent.parent/'Inc/lora.h').read_text() else []),
                    '-DDRIVER_SOURCE="'+str(driver)+'"',
                    str(tests/'test_radio.c'),'-o',str(binary)],check=True)
    for case in cases:
        r=subprocess.run([str(binary),case],capture_output=True,text=True)
        expected=a.expect_delayed_failure and case=='delayed_tx_done'
        if expected and r.returncode==1:
            print('EXPECTED FAILURE',case,r.stderr.strip())
        elif r.returncode:
            print('FAIL',case,r.stdout,r.stderr)
            failed.append(case)
        elif expected:
            print('FAIL expected regression no longer reproduces:',case)
            failed.append(case)
        else:
            print(r.stdout.strip())
print(f'{len(cases)} tests executed; {len(failed)} unexpected failures')
raise SystemExit(bool(failed))
