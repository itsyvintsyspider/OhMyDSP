# 2026-11-06 — Phase 3 step 3.1, attempt 1: guard aborted, nothing sent

Boot-args `dmic_p2=1 dmic_p3_n=1`. Raw: `logs/phase3-1-ioreg.txt`. FW booted to FW_READY, then `P3.1 ABORT: manifest mailbox windows unexpected (down=0x80000/0x0 up=0x0/0x0), not sending`.
Teardown clean, ADSPCS back to 0x00000f0f. **No IPC was sent**; the safety check did what it was for.
Cause: my C parser of the ext-manifest window table used a 28-byte element; `struct sof_ipc_hdr` is 4 bytes (size only, header.h:143), so
`sof_ipc_window_elem` is 24 bytes. Verified offline against the real file: REGS 0x80000, UPBOX 0x81000, DOWNBOX 0xA0000 (0x2000), DEBUG 0xC0000,
EXCEPTION 0xC0800, STREAM 0xC1000, TRACE 0xE0000 — identical to recipe §6. Fixed in DSPBoot.cpp; same boot-args, retry as attempt 2.
