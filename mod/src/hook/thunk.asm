; Tail-jump thunks for vtable slots whose signatures we do not want to assume.
;
; The game calls the slot with this in RCX and whatever else in RDX, R8, R9 and
; XMM0 to XMM3, plus stack arguments. The minimap thunk saves every one of
; those, calls a C++ notifier with only RCX, restores them all, and jumps to
; whatever the slot held before us. A jump, not a call: the original returns
; straight to the game's own caller with the stack arguments and return address
; exactly where they were.
;
; Why not a C++ detour with a guessed prototype: the world map update takes a
; float in XMM1, and a C++ function declared with an integer second argument
; would let the compiler clobber XMM1 before forwarding it. This never touches
; a register it did not save.
;
; Stack alignment: on entry RSP is 8 mod 16. push RCX makes it 0 mod 16, and
; 128 bytes keeps it there, so RSP is 16-aligned at the call as the ABI wants.
; The first 32 bytes are the callee's shadow space; saves start at 32.
;
; The camera thunk is smaller still: it stores RCX into a global and jumps. The
; camera object is reallocated during play, so the only way to hold it is to
; take its address from every update call. No call, no register touched.

EXTERN gs_OnMinimapTick:PROC
EXTERN gs_minimapOriginal:QWORD
EXTERN gs_cameraOriginal:QWORD
EXTERN gs_cameraThis:QWORD
EXTERN gs_cameraCalls:QWORD
EXTERN gs_OnAlert:PROC
EXTERN gs_alertOriginal:QWORD

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

; The alert system's slot 144. Same idea as the minimap thunk and the same
; reason: the signature is not known, so nothing is assumed about it. Every
; register the ABI lets the game pass is saved, the notifier is called with the
; four in registers plus a pointer to the caller's fifth argument, everything is
; put back, and the original runs with its stack untouched.
;
; The fifth argument's address: on entry RSP points at the return address and
; the first stack argument sits at RSP+40. Push RCX and subtract 176 and that
; same slot is at RSP+224.
;
; Stack layout of the 176 bytes: the callee's shadow space at 0 to 31, the
; notifier's fifth argument at 32, the saved registers from 40.
gs_AlertThunk PROC
    push    rcx
    sub     rsp, 176
    mov     [rsp+40], rdx
    mov     [rsp+48], r8
    mov     [rsp+56], r9
    movups  [rsp+64], xmm0
    movups  [rsp+80], xmm1
    movups  [rsp+96], xmm2
    movups  [rsp+112], xmm3
    lea     rax, [rsp+224]
    mov     [rsp+32], rax
    call    gs_OnAlert
    movups  xmm3, [rsp+112]
    movups  xmm2, [rsp+96]
    movups  xmm1, [rsp+80]
    movups  xmm0, [rsp+64]
    mov     r9, [rsp+56]
    mov     r8, [rsp+48]
    mov     rdx, [rsp+40]
    add     rsp, 176
    pop     rcx
    jmp     qword ptr [gs_alertOriginal]
gs_AlertThunk ENDP

gs_CameraThunk PROC
    mov     qword ptr [gs_cameraThis], rcx
    inc     qword ptr [gs_cameraCalls]
    jmp     qword ptr [gs_cameraOriginal]
gs_CameraThunk ENDP

END
