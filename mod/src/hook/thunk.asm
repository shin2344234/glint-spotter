; Tail-jump thunk for a vtable slot whose signature we do not want to assume.
;
; The game calls the slot with this in RCX and whatever else in RDX, R8, R9 and
; XMM0 to XMM3, plus stack arguments. This saves every one of those, calls a C++
; notifier with only RCX, restores them all, and jumps to whatever the slot held
; before us. A jump, not a call: the original returns straight to the game's own
; caller with the stack arguments and return address exactly where they were.
;
; Why not a C++ detour with a guessed prototype: the world map update takes a
; float in XMM1, and a C++ function declared with an integer second argument
; would let the compiler clobber XMM1 before forwarding it. This never touches
; a register it did not save.
;
; Stack alignment: on entry RSP is 8 mod 16. push RCX makes it 0 mod 16, and
; 128 bytes keeps it there, so RSP is 16-aligned at the call as the ABI wants.
; The first 32 bytes are the callee's shadow space; saves start at 32.

EXTERN gs_OnMinimapTick:PROC
EXTERN gs_minimapOriginal:QWORD

.code

gs_MinimapTickThunk PROC
    push    rcx
    sub     rsp, 128
    mov     [rsp+32], rdx
    mov     [rsp+40], r8
    mov     [rsp+48], r9
    movups  [rsp+64], xmm0
    movups  [rsp+80], xmm1
    movups  [rsp+96], xmm2
    movups  [rsp+112], xmm3
    call    gs_OnMinimapTick
    movups  xmm3, [rsp+112]
    movups  xmm2, [rsp+96]
    movups  xmm1, [rsp+80]
    movups  xmm0, [rsp+64]
    mov     r9, [rsp+48]
    mov     r8, [rsp+40]
    mov     rdx, [rsp+32]
    add     rsp, 128
    pop     rcx
    jmp     qword ptr [gs_minimapOriginal]
gs_MinimapTickThunk ENDP

END
