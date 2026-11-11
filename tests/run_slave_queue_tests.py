#!/usr/bin/env python3
"""Compile the actual slave QueueReply/Radio_Process functions with radio stubs."""
from pathlib import Path
import subprocess
import tempfile
import zipfile
root=Path(__file__).resolve().parents[1]
source=(root/'firmware-overrides/relay32/Core/Src/main.c').read_text()
start=source.index('static void QueueReply(')
end=source.index('static void Display_Update(',start)
with tempfile.TemporaryDirectory(prefix='lora-slave-queue-') as tmp:
    tmp=Path(tmp)
    (tmp/'slave_radio.inc').write_text(source[start:end])
    with zipfile.ZipFile(root/'relay32 (2).zip') as z:
        for name in ['protocol.h','node_state.h','app_config.h']:
            (tmp/name).write_bytes(z.read('relay32 (2)/Core/Inc/'+name))
    includes=[tmp]
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*[x for p in includes for x in ['-I',str(p)]],str(root/'tests/test_slave_queue.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
