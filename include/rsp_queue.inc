########################################################
# RSP_QUEUE.INC - Include this file before any text or data segment
# to write an overlay that is compatible with the rsp queue engine.
########################################################

#ifndef RSP_QUEUE_INC
#define RSP_QUEUE_INC

#include <rsp.inc>
#include <rspq_constants.h>
#include <rdpq_constants.h>

########################################################
#
# HOW TO WRITE AN OVERLAY:
#
# 1. Put `#include <rsp_queue.inc>` before any text or data
#    segments in your file.
# 2. Define the overlay header using RSPQ_BeginOverlayHeader
#    and RSPQ_EndOverlayHeader. The header must be placed at the
#    very beginning of your data segment.
# 3. Define some commands by placing calls to RSPQ_DefineCommand
#    in the header, one for each command. The functions that
#    your commands jump to can be located anywhere in your text segment.
#    At the end of each command function, you need to hand control
#    back over to the queue engine.
# 4. In the body of the function, respect the following rules with 
#    respects to register usage:
#    * The first 4 arguments of the function will be passed in
#      registers a0-a3. If your command needs more, you can use
#      the CMD_ADDR macro to fetch them (they are in DMEM but
#      need to be read). You can reuse a0-a3 as you wish in the
#      function body.
#    * "ra" regiser will point to RSPQ_Loop, which is the return point.
#      You can use "jr ra" to go back to the main loop, or simply
#      "j RSPQ_Loop" to jump there.
#    * "gp" register is the only reserved scalar register. If you
#      need to use it, you must save and restore its value
#      before jumping back to RSPQ_Loop.
#    * "$v00" register (alias: vzero) is initialized with 0. It is
#      used by the vcopy macro and is in general useful to have
#      around, but you can reuse it if you wish and there is no
#      need to restore it: the main loop will always zero it before
#      calling each command.
#    * "$v30" and "$v31" registers (alias: vshift and vshift8) contains
#      all powers of two (from 2 to 32768). The single elements are
#      also aliased using "KNN" macros (eg: K16 is vshift.e3). In addition
#      to being often useful in many calculations, they are also used
#      by the vector shift macros defined in rsp.inc (vsll, vsra, vsrl).
#      If you want, you can clobber these registers (obviously the macros
#      will not work anymore), and there is no need to restore them:
#      the main loop will always initialize them before calling each command.
# 5. This is how to use DMEM:
#    * The bss segment is useful to define empty space to use as buffers
#      (eg: to store data read via DMA). NOTE: contrary to C, the bss
#      segment is not initialized to zero, and is not preserved between
#      commands. Assume it contains random data when each command is called.
#    * The data segment is useful to define constant data that you might need
#      in the implementation of command. While the whole DMEM is also
#      writable, please do not do so, and assume that the data segment
#      might be restored to its original state at any time.
#    * Somewhere in your data segment, define the saved overlay
#      state using RSPQ_BeginSavedState and RSPQ_EndSavedState.
#      The save state is preserved across all calls to the overlay,
#      that is across different commands. This is the only part of
#      the data segement that you should consider writable.
#      NOTE: make sure to put at least one data directive that emits
#      a non-zero amount of bytes between them. If your overlay
#      doesn't need any data to be persisted, then use
#      RSPQ_EmptySavedState instead.
# 
# Read below for more details on how to use the macros mentioned above.
#
########################################################

########################################################
# RSPQ_BeginOverlayHeader
# 
# Begins the definition of an overlay header. Must be
# followed by at least one call to RSPQ_DefineCommand
# and closed with RSPQ_EndOverlayHeader. Only calls
# to RSPQ_DefineCommand are allowed inside the header definition.
########################################################
.macro RSPQ_BeginOverlayHeader
# This reflects the rspq_overlay_header_t struct defined in rspq.c
_RSPQ_OVERLAY_HEADER:
    # state start
    .short _RSPQ_SAVED_STATE_START - _data_start
    # state size
    .short _RSPQ_SAVED_STATE_END - _RSPQ_SAVED_STATE_START - 1
    # command base (filled in by C code)
    .short 0
    # unused padding
    .short 0
    
	.align 1
_RSPQ_OVERLAY_COMMAND_TABLE:
.endm

########################################################
# RSPQ_BeginSavedState
# 
# Begins the definition of the saved overlay state. Must be
# closed by RSPQ_EndSavedState. The region of memory between 
# RSPQ_BeginSavedState and RSPQ_EndSavedState will be preserved 
# by the queue engine across all calls to an overlay, even if 
# it gets unloaded from memory.
#
# Every overlay must define exactly one saved state and it
# must contain at least one data directive or, in other words,
# its size must not be zero.
########################################################
.macro RSPQ_BeginSavedState
    .align 4
_RSPQ_SAVED_STATE_START:
.endm

########################################################
# RSPQ_EndSavedState
# 
# Ends the definition of the saved overlay state.
########################################################
.macro RSPQ_EndSavedState
    .align 3
_RSPQ_SAVED_STATE_END:
.endm

########################################################
# RSPQ_EmptySavedState
# 
# Defines an empty saved overlay state. Use this instead
# of RSPQ_BeginSavedState and RSPQ_EndSavedState if your
# overlay does not need any persistent state.
#
# It is only possible to use either this or 
# RSPQ_BeginSavedState/RSPQ_EndSavedState, but not both.
# Do not use RSPQ_BeginSavedState and RSPQ_EndSavedState
# without any data directives between them.
########################################################
.macro RSPQ_EmptySavedState
    RSPQ_BeginSavedState
    .quad 0
    RSPQ_EndSavedState
.endm

########################################################
# RSPQ_DefineCommand
# 
# Registers a command to be callable by the rsp queue engine.
# The size of the command specifies how much physical memory
# within the queue an instance of the command will occupy 
# when it is queued. This effectively dictates how many "parameters"
# the command can carry.
#
# Note that the first 4 words (16 bytes) of every command are
# always prefetched into registers a0-a3 before it is jumped to.
#
# ARGS:
#   function: Address of the function that will be jumped to
#             when this command is executed.
#   size:     The size of the command in bytes. Must be a 
#             multiple of 4 and in the range [0, RSPQ_DESCRIPTOR_MAX_SIZE].
########################################################
.macro RSPQ_DefineCommand function size
    .ifne ((\size) % 4)
        .error "Invalid size - must be multiple of 4"
        .exitm
    .endif

    .iflt (\size)
        .error "Invalid size - valid range: [0, RSPQ_DESCRIPTOR_MAX_SIZE]"
        .exitm
    .endif

    .ifgt ((\size) - RSPQ_DESCRIPTOR_MAX_SIZE)
        .error "Invalid size - valid range: [0, RSPQ_DESCRIPTOR_MAX_SIZE]"
        .exitm
    .endif

    # Put the command size (as number of 4 byte words) into the high 6 bits,
    # and the jump address shifted right by 2 bits into the lower 10.
    .short (\function - _start) >> 2 | ((\size) & RSPQ_DESCRIPTOR_SIZE_MASK) << 8
.endm

########################################################
# RSPQ_EndOverlayHeader
# 
# Ends the definition of an overlay header.
########################################################
.macro RSPQ_EndOverlayHeader
    .short 0
.endm

########################################################
# Global register allocations
#
# rspq does only reserver one register (gp), which needs
# to be preserved by overlays.
# 
# It then initializes a few registers to save some work
# from overlays, but they can be freely reused.
#
########################################################

# Globally reserved register. Do not use it in overlay code
#define rspq_dmem_buf_ptr gp

# This register will be set to the command's size in bytes when jumping to the
# command function. It can be reused freely if the function does not need it.
#define rspq_cmd_size t7

# This macro can be used with l* instructions to get contents of the current
# command at the specified offset.
# The total command size needs to be specified as well.
#define CMD_ADDR(offset, cmdsize) (%lo(RSPQ_DMEM_BUFFER) + (offset) - (cmdsize)) (rspq_dmem_buf_ptr)

# This register is initialized to zero any time a command is called
#define vzero    $v00

# These registers are initialized with the constant data required to make
# vector shift macros code works (power of twos).
#define vshift   $v30
#define vshift8  $v31

# vcopy macro: copy one vector register to another. This requires
# vzero to be initialized to zero, which is true within rspq.
.macro vcopy dest, source
    vor \dest, vzero, \source
.endm

# We also define direct access to small constants as they can be useful in some
# calculations.
#define K1       vshift.e7
#define K2       vshift.e6
#define K4       vshift.e5
#define K8       vshift.e4
#define K16      vshift.e3
#define K32      vshift.e2
#define K64      vshift.e1
#define K128     vshift.e0
#define K256     vshift8.e7
#define K512     vshift8.e6
#define K1024    vshift8.e5
#define K2048    vshift8.e4
#define K4096    vshift8.e3
#define K8192    vshift8.e2
#define K16384   vshift8.e1
#define K32768   vshift8.e0
#define KM32768  vshift8.e0    // note: this is actually -32768 for most arithmetic operations


########################################################
#
# The following is the actual implementation of the rsp engine.
# It is included in every overlay to verify binary
# compatibility at runtime during overlay registration. 
# But only the overlay code will actually be loaded into 
# the RSP during execution.
#
########################################################

#define OVERLAY_HEADER_SIZE 0x8

    .set noreorder
    .set at

    .data
_data_start:

# Data for vector shift registers.
# We put this at the top of the DMEM as we need an absolute address to save one opcode.
    vsll_data
    vsll8_data

# Overlay tables. See rsp_overlay_t in rsp.c
RSPQ_OVERLAY_TABLE:           .ds.b RSPQ_OVERLAY_TABLE_SIZE
RSPQ_OVERLAY_DESCRIPTORS:     .ds.b (RSPQ_OVERLAY_DESC_SIZE * RSPQ_MAX_OVERLAY_COUNT)

# Save slots for RDRAM addresses used during nested lists calls.
# Notice that the two extra slots are used to save the lowpri
# and highpri current pointer (used when switching between the two)
RSPQ_POINTER_STACK:           .ds.l (RSPQ_MAX_BLOCK_NESTING_LEVEL+2)

# RDRAM address of the current command list.
RSPQ_RDRAM_PTR:               .long 0

################################################################
# RDPQ shared state
#
# The next variables are state managed by rsp_rdpq.inc.
# Not all overlays need rsp_rdpq.inc, but those that do, need
# this state to be shared among all of them. We don't have
# a way to do that currently, so the best workaround is to
# keep this shared state in the rspq state itself, which is
# always loaded in DMEM. This wastes some DMEM for overlays
# that don't need to generate RDP commands, but it's the least
# worse solution we can currently come up with.
################################################################

# Pointer to the end of the current RDP output buffer
RDPQ_SENTINEL:              .long 0

# RDP MODE (32 bytes). NOTE: This must be 16-byte aligned (because we use lqv in push/pop),
# but we can't use .align 4 here, otherwise it's not easy to keep this structure layout
# in sync with the C side (rdpq_state_t in rdpq.c).
RDPQ_MODE:
    # Combiner formula set by the application (1pass or 2pass)
    RDPQ_COMBINER:               .quad  0
    RDPQ_COMBINER_MIPMAPMASK:    .quad  0
    # Blender settings: up to two steps. Either of them
    # is already in a format valid for both 1cyc and 2cyc mode.
    RDPQ_MODE_BLENDER_STEPS:     .word  0,0
    # Curent state of "Set Other Modes" RDP mode command, plus our own extension
    # fields (see SOMX_* in rdpq_macros.h). Notice that the top byte also contains
    # extensions fields, so when sending this command to RDP, the top byte must be
    # changed with the SOM command ID (0xEF) in the RDP output buffer.
    RDPQ_OTHER_MODES:            .quad  0
RDPQ_MODE_END:

# Current scissor rectangle (in RDP commmand format)
RDPQ_SCISSOR_RECT:           .quad 0
# Two RDP output buffers (to alternate between)
RDPQ_DYNAMIC_BUFFERS:        .long 0, 0
# Current RDP write pointer (8 MSB are garbage)
RDPQ_CURRENT:                .long 0
# Current fill color
RDPQ_FILL_COLOR:             .word  0
# Current target buffer color depth
RDPQ_TARGET_BITDEPTH:        .byte  0
# True if there is a SYNC_FULL command in the current RDP output buffer
RDPQ_SYNCFULL_ONGOING:       .byte  0
# True if the rdpq debug mode is active (validator). TODO: hide this bit elsewhere to save one byte
RDPQ_DEBUG:                  .byte 0

################################################################
# End of RDPQ shared state
################################################################

# Index (not ID!) of the current overlay, as byte offset in the descriptor array
RSPQ_CURRENT_OVL:             .half 0

    .align 4
    .ascii "Dragon RSP Queue"
    .ascii "Rasky & Snacchus"

    .align 3
RSPQ_INTERNAL_COMMAND_TABLE:
RSPQ_DefineCommand RSPQCmd_WaitNewInput,    0     # 0x00
RSPQ_DefineCommand RSPQCmd_Noop,            4     # 0x01
RSPQ_DefineCommand RSPQCmd_Jump,            4     # 0x02
RSPQ_DefineCommand RSPQCmd_Call,            8     # 0x03
RSPQ_DefineCommand RSPQCmd_Ret,             4     # 0x04
RSPQ_DefineCommand RSPQCmd_Dma,             16    # 0x05
RSPQ_DefineCommand RSPQCmd_WriteStatus,     4     # 0x06 -- must be even (bit 24 must be 0)
RSPQ_DefineCommand RSPQCmd_SwapBuffers,     12    # 0x07
RSPQ_DefineCommand RSPQCmd_TestWriteStatus, 8     # 0x08 -- must be even (bit 24 must be 0)
RSPQ_DefineCommand RSPQCmd_RdpWaitIdle,     4     # 0x09
RSPQ_DefineCommand RSPQCmd_RdpSetBuffer,    12    # 0x0A
RSPQ_DefineCommand RSPQCmd_RdpAppendBuffer, 4     # 0x0B

    .align 3
#if RSPQ_DEBUG
                             .long 0, RSPQ_DEBUG_MARKER
#endif
RSPQ_DMEM_BUFFER:            .ds.b RSPQ_DMEM_BUFFER_SIZE


    .align 4
# Overlay data will be loaded at this address
_ovl_data_start:

    .text

    # Just declare the shift macros, without emitting code. We will be emitting it later
    setup_vsll      vshift, 0
    setup_vsll8    vshift8, 0

    .globl _start
_start:
    li rspq_dmem_buf_ptr, 0

    .func RSPQCmd_WaitNewInput
RSPQCmd_WaitNewInput:
    # Check if new commands were added in the display list (SIG_MORE)
    mfc0 t0, COP0_SP_STATUS
    andi t0, SP_STATUS_SIG_MORE
    bnez t0, wakeup
    li t0, SP_WSTATUS_CLEAR_SIG_MORE

    # No new commands yet, go to sleep
    break
wakeup:
    mtc0 t0, COP0_SP_STATUS

rspq_fetch_buffer:
    # Fetch the RDRAM pointer, and adjust it to the current reading index.
    # We will fetch commands starting from there
    lw s0, %lo(RSPQ_RDRAM_PTR)
    add s0, rspq_dmem_buf_ptr
rspq_fetch_buffer_with_ptr:
    # Buffer into which the DMA will be performed
    # NOTE: this instruction has been moved here to workaround what
    # seems to be a RSP hardware bug, related to weird pipeline
    # behavior during RSP un-halt. If you move this opcode later
    # just before "jal DMAIn", bruteforce tests in testrom will start
    # to fail. Unfortunately, we are still not able to isolate this
    # bug.
    li s4, %lo(RSPQ_DMEM_BUFFER)

    # Reset the reading index to the first actual byte of the buffer (after
    # taking misalignment into account)
    andi rspq_dmem_buf_ptr, s0, 7

    # Correction for misalignment
    sub s0, rspq_dmem_buf_ptr

    # Store the updated pointer
    sw s0, %lo(RSPQ_RDRAM_PTR)

    # Fetch the whole DMEM buffer. We will use the 0x00 invalid fill value to
    # realize where the buffer is empty.
    jal DMAIn
    li t0, DMA_SIZE(RSPQ_DMEM_BUFFER_SIZE, 1)

    # fallthrough into the main loop
    .endfunc

RSPQCmd_Noop:     # noop -> repeat the loop
    .func RSPQ_Loop
RSPQ_Loop:
    #define ovl_index t4
    #define cmd_index t5    // referenced in rspq_assert_invalid_overlay
    #define cmd_desc  t6

    jal RSPQ_CheckHighpri
    li t0, 0

    # Read first word
    lw a0, %lo(RSPQ_DMEM_BUFFER) + 0x0 (rspq_dmem_buf_ptr)

    # Index into overlay table
    srl t0, a0, 28

    # Index into command table
    srl cmd_index, a0, 23
    andi cmd_index, 0x1FE

    # Overlay 0 is reserved for internal commands
    beqz t0, rspq_execute_command
    # Load command descriptor from internal command table if using the default overlay.
    # Otherwise, cmd_desc will be overwritten further down
    lhu cmd_desc, %lo(RSPQ_INTERNAL_COMMAND_TABLE)(cmd_index)

    lhu t1, %lo(RSPQ_CURRENT_OVL)

    # Load overlay index from overlay table
    lbu ovl_index, %lo(RSPQ_OVERLAY_TABLE)(t0)
    #if RSPQ_DEBUG
    assert_ne ovl_index, 0, ASSERT_INVALID_OVERLAY
    #endif
    # Check if the requested overlay is already in memory
    beq ovl_index, t1, rspq_overlay_loaded
    lhu t0, %lo(_ovl_data_start) + 0x2

    # Save current overlay state
    lw s0, %lo(RSPQ_OVERLAY_DESCRIPTORS) + 0x8 (t1)
    jal DMAOutAsync
    lhu s4, %lo(_ovl_data_start) + 0x0

    # Load overlay data (saved state is included)
    lhu t0, %lo(RSPQ_OVERLAY_DESCRIPTORS) + 0xE (ovl_index)
    lw s0, %lo(RSPQ_OVERLAY_DESCRIPTORS) + 0x4 (ovl_index)
    jal DMAInAsync
    li s4, %lo(_ovl_data_start)

    # Load overlay code
    lhu t0, %lo(RSPQ_OVERLAY_DESCRIPTORS) + 0xC (ovl_index)
    lw s0, %lo(RSPQ_OVERLAY_DESCRIPTORS) + 0x0 (ovl_index)
    jal DMAIn
    li s4, %lo(_ovl_text_start - _start) + 0x1000

    # Remember loaded overlay
    sh ovl_index, %lo(RSPQ_CURRENT_OVL)

rspq_overlay_loaded:
    # Subtract the command base to determine the final offset into the command table.
    lhu t0, %lo(_ovl_data_start) + 0x4
    sub cmd_index, t0

    # Load command descriptor from overlay command table
    lhu cmd_desc, %lo(_ovl_data_start) + OVERLAY_HEADER_SIZE(cmd_index)

rspq_execute_command:
    #if RSPQ_DEBUG
    assert_ne cmd_desc, 0, ASSERT_INVALID_COMMAND
    #endif

    # Command size
    srl rspq_cmd_size, cmd_desc, 8
    andi rspq_cmd_size, RSPQ_DESCRIPTOR_SIZE_MASK

    # Check if the command is truncated because of buffer overflow (that is,
    # it finishes beyond the buffer end). If so, we must refetch the buffer
    # starting from the current position.
    # Notice that we use "bge" instead of "bgt" so we actually refetch the buffer
    # also if the current command ends exactly at buffer end; this is slighly
    # wasteful but saves us a few instructions (that would be required to check
    # whether we are then trying to load a command outside of the buffer).
    addu t0, rspq_dmem_buf_ptr, rspq_cmd_size
    bge t0, RSPQ_DMEM_BUFFER_SIZE, rspq_fetch_buffer

    # Load second to fourth command words (might be garbage, but will never be read in that case)
    # This saves some instructions in all overlays that use more than 4 bytes per command.
    lw a1, %lo(RSPQ_DMEM_BUFFER) + 0x4 (rspq_dmem_buf_ptr)
    lw a2, %lo(RSPQ_DMEM_BUFFER) + 0x8 (rspq_dmem_buf_ptr)
    lw a3, %lo(RSPQ_DMEM_BUFFER) + 0xC (rspq_dmem_buf_ptr)
    add rspq_dmem_buf_ptr, rspq_cmd_size

    # Initialize vzero, vshift, vshift8.
    vxor vzero, vzero,0
    lqv vshift,  0x00,zero
    lqv vshift8, 0x10,zero

    # Jump to command. Set ra to the loop function, so that commands can 
    # either do "j RSPQ_Loop" or "jr ra" (or a tail call) to get back to the main loop
    sll cmd_desc, 2
    jr cmd_desc
    li ra, %lo(RSPQ_Loop)

    #undef ovl_index
    #undef cmd_index
    #undef cmd_desc
    .endfunc

    ############################################################
    # RSPQ_CheckHighpri
    #
    # Polling function. Check whether the highpri mode has been
    # requested by the CPU, and if so start executing it right away.
    #
    # This is called by the main loop automatically between each
    # command, but can be also polled by any overlay function
    # that takes a long time and want to yield. In this case, 
    # the same command will be executed again when the highpri queue
    # is finished, so make sure there is state to continue rather
    # than restart the execution.
    #
    # ARGS:
    #   t0: size of the current command
    ############################################################

    .func RSPQ_CheckHighpri
RSPQ_CheckHighpri:
    # We need to enter high-pri mode if highpri was requested and it is not
    # already running (to avoid infinite loops).
    mfc0 t2, COP0_SP_STATUS
    andi t2, SP_STATUS_SIG_HIGHPRI_REQUESTED | SP_STATUS_SIG_HIGHPRI_RUNNING
    bne t2, SP_STATUS_SIG_HIGHPRI_REQUESTED, JrRa

    # Switch to highpri mode, calling RSPQCMD_SwapBuffers. We prepare the
    # argument to switch to the highpri buffer, clears the highpri requested
    # signal, and set the highpri running signal.
    li a2, SP_WSTATUS_CLEAR_SIG_HIGHPRI_REQUESTED | SP_WSTATUS_SET_SIG_HIGHPRI_RUNNING
    li a0, RSPQ_HIGHPRI_CALL_SLOT<<2
    li a1, RSPQ_LOWPRI_CALL_SLOT<<2
    sub rspq_dmem_buf_ptr, t0
    #fallthrough
    .endfunc

    #############################################################
    # RSPQCmd_SwapBuffers
    #
    # Switch between lowpri and highpri or viceversa. This is
    # called by RSP itself to go into highpri mode, and scheduled
    # as normal command by CPU when going back into lowpri.
    #
    # ARGS:
    #   a0: Pointer stack slot that contains the address to switch to.
    #       (either RSPQ_LOWPRI_CALL_SLOT<<2 or RSPQ_HIGHPRI_CALL_SLOT<<2)
    #   a1: Pointer stack slot where to save the current address to.
    #       (either RSPQ_LOWPRI_CALL_SLOT<<2 or RSPQ_HIGHPRI_CALL_SLOT<<2)
    #   a2: New mask to check for HIGHPRI signal (0 in highpri mode).
    #############################################################
    .func RSPQCmd_SwapBuffers
RSPQCmd_SwapBuffers:
    mtc0 a2, COP0_SP_STATUS
    lw a0, %lo(RSPQ_POINTER_STACK)(a0)
    #fallthrough
    .endfunc    
    
    #############################################################
    # RSPQCmd_Call
    #
    # Do a nested call to a different command list. Save the
    # current RDRAM position to be able to resume execution
    # later.
    #
    # ARGS:
    #   a0: New RDRAM address (plus command opcode)
    #   a1: DMEM address of the save slot for the current address
    #############################################################
    .func RSPQCmd_Call
RSPQCmd_Call:
    # a0: command opcode + RDRAM address
    # a1: call slot in DMEM
    lw s0, %lo(RSPQ_RDRAM_PTR)
    add s0, rspq_dmem_buf_ptr
    sw s0, %lo(RSPQ_POINTER_STACK)(a1)  # save return address
    # fallthrough
    .endfunc

    #############################################################
    # RSPQCmd_Jump
    #
    # Jump to a different command list. The current RDRAM position
    # is lost.
    #
    # ARGS:
    #   a0: New RDRAM address (plus command opcode)
    #############################################################
    .func RSPQCmd_Jump
RSPQCmd_Jump:
    # a0: command opcode + RDRAM address
    j rspq_fetch_buffer_with_ptr
    move s0, a0
    .endfunc

    #############################################################
    # RSPQCmd_Ret
    #
    # Restore a previously saved RDRAM position and jump to it.
    #
    # ARGS:
    #   a0: DMEM address of the save slot (plus command opcode)
    #############################################################
    .func RSPQCmd_Ret
RSPQCmd_Ret:
    # a0: command opcode + call slot in DMEM to recover
    j rspq_fetch_buffer_with_ptr
    lw s0, %lo(RSPQ_POINTER_STACK)(a0)
    .endfunc

    #############################################################
    # RSPQCmd_TestWriteStatus
    #
    # Like RSPQCmd_WriteStatus, writes COP0 SP status register with a specified value,
    # but first waits until the SP status AND'ed with the specified bitmask is zero.
    #
    # ARGS:
    #   a0: value to write into COP0_SP_STATUS
    #   a1: bitmask to test COP0_SP_STATUS for
    #############################################################
    .func RSPQCmd_TestWriteStatus
RSPQCmd_TestWriteStatus:
    jal SpStatusWait
    move t2, a1
    # fallthrough
    .endfunc

    #############################################################
    # RSPQCmd_WriteStatus
    #
    # Write COP0 SP status register with a specified value
    #
    # ARGS:
    #   a0: value to write into COP0_SP_STATUS
    #############################################################
    .func RSPQCmd_WriteStatus
RSPQCmd_WriteStatus:
    # Write the status value and go back to the main loop.
    # Notice that we write the whole a0 register, whose top 8-bits
    # contain the RSPQ_CMD_WRITE_STATUS command ID. This is not
    # a problem because the SP_STATUS register uses only 25 bits,
    # and we have assured that RSPQ_CMD_WRITE_STATUS has an ID
    # which is an even number (so that bit 24 is always 0 and not used).
    # This means that this command cannot ever write bit 24 to SP_STATUS:
    # that is SP_WSTATUS_SET_SIG_MORE, that we don't need to set
    # through this internal command.
    j RSPQ_Loop
    mtc0 a0, COP0_SP_STATUS
    .endfunc

    #############################################################
    # RSPQCmd_Dma
    #
    # Executes an arbitrary DMA request by taking the arguments to DMAExec from a0-a3
    #
    # ARGS:
    #   a0: RDRAM address
    #   a1: DMEM address
    #   a2: length/height
    #   a3: flags (in/out, sync/async)
    #############################################################
    .func RSPQCmd_Dma
RSPQCmd_Dma:
    move s0, a0
    move s4, a1
    move t0, a2
    j DMAExec
    move t2, a3
    .endfunc

    #############################################################
    # RSPQCmd_RdpSetBuffer
    # 
    # Configure a new output buffer in RDRAM for RDP. This can be
    # used to either send a buffer of commands to RDP, or to setup
    # a buffer for generation of new RDP commands.
    # In fact, a1/a0 will be written to DP_START/DP_END, so if
    # DP_END > DP_START, the commands contained in the buffer will
    # be immediately processed by RDP. If DP_START==DP_END, the buffer
    # is basically "stored" in the RDP registers but nothing happens:
    # subsequent calls to RSPQ_RdpSend will append commands to it,
    # until the sentinel (a2) is hit, which means that the buffer is
    # full. At this point, RSPQ_RdpSend will get back to the
    # standard buffers (RDPQ_DYNAMIC_BUFFERS).
    #
    # ARGS:
    #   a0: New end pointer (to write to DP_END)
    #   a1: New start buffer (to write to DP_START)
    #   a2: New sentinel (end of total capacity of the buffer)
    #
    # NOTE: if the sentinel is 0, the command will force a switch
    #       to the RDPQ dynamic buffer (the next one). This will happen
    #       as soon as next RDPQ_Send is run.
    #############################################################
    .func RSPQCmd_RdpSetBuffer
RSPQCmd_RdpSetBuffer:
    # Update the sentinel. If zero, there's nothing more to do: next
    # RDPQ_Send will switch to the next dynamic buffer.
    sw a2, %lo(RDPQ_SENTINEL)
    beqz a2, JrRa
    move ra2, ra

    # Wait for RDP DMA FIFO to be not full. If there's another
    # pending buffer, we cannot do anything but wait.
    lbu t3, %lo(RDPQ_SYNCFULL_ONGOING)
    jal RSPQ_RdpWait
    ori t3, DP_STATUS_END_VALID

    # In case there was a SYNC_FULL pending, before switching to next buffer, we need
    # to actually write the current one to RDP. Notice that the following two instructions
    # are actually nops in case there was no SYNC_FULL (as DP_END == RDPQ_CURRENT in that case).
    sb zero, %lo(RDPQ_SYNCFULL_ONGOING)
    lw t0, %lo(RDPQ_CURRENT)
#ifdef NDEBUG
    mtc0 t0, COP0_DP_END
#else
    # Check if the RDPQ debug mode is active (validator)
    lbu t2, %lo(RDPQ_DEBUG)
    beqz t2, 1f
    mtc0 t0, COP0_DP_END

    # Generate a RSP interrupt to tell the CPU to fetch the new DP_START / DP_END.
    # Then also wait until the CPU has acknowledged fetching the buffer (via SIG0)
    # so that we absolutely don't lose the buffer.
    jal SpStatusWait
    li t2, SP_STATUS_SIG0
    li t0, SP_WSTATUS_SET_SIG0 | SP_WSTATUS_SET_INTR
    mtc0 t0, COP0_SP_STATUS
    jal SpStatusWait
    li t2, SP_STATUS_SIG0
#endif
1:  move ra, ra2

    # Write new start buffer pointer, and fallthrough to 
    # RSPQCmd_RdpAppendBuffer to write the new end pointer
    mtc0 a1, COP0_DP_START
    # fallthrough!
    .endfunc


    #############################################################
    # RSPQCmd_RdpAppendBuffer
    # 
    # Tell RDP to run more commands that were appended to the end
    # of the current RDP output buffer.
    #
    # ARGS:
    #   a0: New end pointer (to write to DP_END)
    #############################################################
    .func RSPQCmd_RdpAppendBuffer
RSPQCmd_RdpAppendBuffer:
    lbu t0, %lo(RDPQ_SYNCFULL_ONGOING)
    bnez t0, JrRa
    sw a0, %lo(RDPQ_CURRENT)
    jr ra
    mtc0 a0, COP0_DP_END
    .endfunc

    #############################################################
    # RSPQCmd_RdpWaitIdle
    #
    # Wait until RDP is idle. 
    # 
    # *NOTE*: a SYNC_FULL command *must* have been already written
    # to the RDP output buffer,  otherwise this function will stall
    # forever. In fact, once the RDP is running, it never gets back
    # to idle state until a SYNC_FULL is found, even if it has no
    # more commands to run.
    #############################################################
    .func RSPQCmd_RdpWaitIdle
RSPQCmd_RdpWaitIdle:
    li t3, DP_STATUS_BUSY

RSPQ_RdpWait:
    mfc0 t2, COP0_DP_STATUS
1:
    # Wait for selected RDP status bits to become 0.
    and t1, t2, t3
    bnez t1, 1b
    mfc0 t2, COP0_DP_STATUS
    jr ra
    nop
    .endfunc

#include <rsp_dma.inc>
#include <rsp_assert.inc>

    .align 3
# Overlay code will be loaded at this address
_ovl_text_start:

#endif
