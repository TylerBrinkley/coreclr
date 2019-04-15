// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                        Arm64 Code Generator                               XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/
#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif

#ifdef _TARGET_ARM64_
#include "emit.h"
#include "codegen.h"
#include "lower.h"
#include "gcinfo.h"
#include "gcinfoencoder.h"

/*
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                           Prolog / Epilog                                 XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

//------------------------------------------------------------------------
// genInstrWithConstant:   we will typically generate one instruction
//
//    ins  reg1, reg2, imm
//
// However the imm might not fit as a directly encodable immediate,
// when it doesn't fit we generate extra instruction(s) that sets up
// the 'regTmp' with the proper immediate value.
//
//     mov  regTmp, imm
//     ins  reg1, reg2, regTmp
//
// Arguments:
//    ins                 - instruction
//    attr                - operation size and GC attribute
//    reg1, reg2          - first and second register operands
//    imm                 - immediate value (third operand when it fits)
//    tmpReg              - temp register to use when the 'imm' doesn't fit. Can be REG_NA
//                          if caller knows for certain the constant will fit.
//    inUnwindRegion      - true if we are in a prolog/epilog region with unwind codes.
//                          Default: false.
//
// Return Value:
//    returns true if the immediate was too large and tmpReg was used and modified.
//
bool CodeGen::genInstrWithConstant(instruction ins,
                                   emitAttr    attr,
                                   regNumber   reg1,
                                   regNumber   reg2,
                                   ssize_t     imm,
                                   regNumber   tmpReg,
                                   bool        inUnwindRegion /* = false */)
{
    bool     immFitsInIns = false;
    emitAttr size         = EA_SIZE(attr);

    // reg1 is usually a dest register
    // reg2 is always source register
    assert(tmpReg != reg2); // regTmp can not match any source register

    switch (ins)
    {
        case INS_add:
        case INS_sub:
            if (imm < 0)
            {
                imm = -imm;
                ins = (ins == INS_add) ? INS_sub : INS_add;
            }
            immFitsInIns = emitter::emitIns_valid_imm_for_add(imm, size);
            break;

        case INS_strb:
        case INS_strh:
        case INS_str:
            // reg1 is a source register for store instructions
            assert(tmpReg != reg1); // regTmp can not match any source register
            immFitsInIns = emitter::emitIns_valid_imm_for_ldst_offset(imm, size);
            break;

        case INS_ldrsb:
        case INS_ldrsh:
        case INS_ldrsw:
        case INS_ldrb:
        case INS_ldrh:
        case INS_ldr:
            immFitsInIns = emitter::emitIns_valid_imm_for_ldst_offset(imm, size);
            break;

        default:
            assert(!"Unexpected instruction in genInstrWithConstant");
            break;
    }

    if (immFitsInIns)
    {
        // generate a single instruction that encodes the immediate directly
        getEmitter()->emitIns_R_R_I(ins, attr, reg1, reg2, imm);
    }
    else
    {
        // caller can specify REG_NA  for tmpReg, when it "knows" that the immediate will always fit
        assert(tmpReg != REG_NA);

        // generate two or more instructions

        // first we load the immediate into tmpReg
        instGen_Set_Reg_To_Imm(size, tmpReg, imm);
        regSet.verifyRegUsed(tmpReg);

        // when we are in an unwind code region
        // we record the extra instructions using unwindPadding()
        if (inUnwindRegion)
        {
            compiler->unwindPadding();
        }

        // generate the instruction using a three register encoding with the immediate in tmpReg
        getEmitter()->emitIns_R_R_R(ins, attr, reg1, reg2, tmpReg);
    }
    return immFitsInIns;
}

//------------------------------------------------------------------------
// genStackPointerAdjustment: add a specified constant value to the stack pointer in either the prolog
// or the epilog. The unwind codes for the generated instructions are produced. An available temporary
// register is required to be specified, in case the constant is too large to encode in an "add"
// instruction (or "sub" instruction if we choose to use one), such that we need to load the constant
// into a register first, before using it.
//
// Arguments:
//    spDelta                 - the value to add to SP (can be negative)
//    tmpReg                  - an available temporary register
//    pTmpRegIsZero           - If we use tmpReg, and pTmpRegIsZero is non-null, we set *pTmpRegIsZero to 'false'.
//                              Otherwise, we don't touch it.
//    reportUnwindData        - If true, report the change in unwind data. Otherwise, do not report it.
//
// Return Value:
//    None.

void CodeGen::genStackPointerAdjustment(ssize_t spDelta, regNumber tmpReg, bool* pTmpRegIsZero, bool reportUnwindData)
{
    // Even though INS_add is specified here, the encoder will choose either
    // an INS_add or an INS_sub and encode the immediate as a positive value
    //
    if (genInstrWithConstant(INS_add, EA_PTRSIZE, REG_SPBASE, REG_SPBASE, spDelta, tmpReg, true))
    {
        if (pTmpRegIsZero != nullptr)
        {
            *pTmpRegIsZero = false;
        }
    }

    if (reportUnwindData)
    {
        // spDelta is negative in the prolog, positive in the epilog, but we always tell the unwind codes the positive
        // value.
        ssize_t  spDeltaAbs    = abs(spDelta);
        unsigned unwindSpDelta = (unsigned)spDeltaAbs;
        assert((ssize_t)unwindSpDelta == spDeltaAbs); // make sure that it fits in a unsigned

        compiler->unwindAllocStack(unwindSpDelta);
    }
}

//------------------------------------------------------------------------
// genPrologSaveRegPair: Save a pair of general-purpose or floating-point/SIMD registers in a function or funclet
// prolog. If possible, we use pre-indexed addressing to adjust SP and store the registers with a single instruction.
// The caller must ensure that we can use the STP instruction, and that spOffset will be in the legal range for that
// instruction.
//
// Arguments:
//    reg1                     - First register of pair to save.
//    reg2                     - Second register of pair to save.
//    spOffset                 - The offset from SP to store reg1 (must be positive or zero).
//    spDelta                  - If non-zero, the amount to add to SP before the register saves (must be negative or
//                               zero).
//    useSaveNextPair          - True if the last prolog instruction was to save the previous register pair. This
//                               allows us to emit the "save_next" unwind code.
//    tmpReg                   - An available temporary register. Needed for the case of large frames.
//    pTmpRegIsZero            - If we use tmpReg, and pTmpRegIsZero is non-null, we set *pTmpRegIsZero to 'false'.
//                               Otherwise, we don't touch it.
//
// Return Value:
//    None.

void CodeGen::genPrologSaveRegPair(regNumber reg1,
                                   regNumber reg2,
                                   int       spOffset,
                                   int       spDelta,
                                   bool      useSaveNextPair,
                                   regNumber tmpReg,
                                   bool*     pTmpRegIsZero)
{
    assert(spOffset >= 0);
    assert(spDelta <= 0);
    assert((spDelta % 16) == 0);                                  // SP changes must be 16-byte aligned
    assert(genIsValidFloatReg(reg1) == genIsValidFloatReg(reg2)); // registers must be both general-purpose, or both
                                                                  // FP/SIMD

    bool needToSaveRegs = true;
    if (spDelta != 0)
    {
        assert(!useSaveNextPair);
        if ((spOffset == 0) && (spDelta >= -512))
        {
            // We can use pre-indexed addressing.
            // stp REG, REG + 1, [SP, #spDelta]!
            // 64-bit STP offset range: -512 to 504, multiple of 8.
            getEmitter()->emitIns_R_R_R_I(INS_stp, EA_PTRSIZE, reg1, reg2, REG_SPBASE, spDelta, INS_OPTS_PRE_INDEX);
            compiler->unwindSaveRegPairPreindexed(reg1, reg2, spDelta);

            needToSaveRegs = false;
        }
        else // (spOffset != 0) || (spDelta < -512)
        {
            // We need to do SP adjustment separately from the store; we can't fold in a pre-indexed addressing and the
            // non-zero offset.

            // generate sub SP,SP,imm
            genStackPointerAdjustment(spDelta, tmpReg, pTmpRegIsZero, /* reportUnwindData */ true);
        }
    }

    if (needToSaveRegs)
    {
        // stp REG, REG + 1, [SP, #offset]
        // 64-bit STP offset range: -512 to 504, multiple of 8.
        assert(spOffset <= 504);
        getEmitter()->emitIns_R_R_R_I(INS_stp, EA_PTRSIZE, reg1, reg2, REG_SPBASE, spOffset);

        if (useSaveNextPair)
        {
            // This works as long as we've only been saving pairs, in order, and we've saved the previous one just
            // before this one.
            compiler->unwindSaveNext();
        }
        else
        {
            compiler->unwindSaveRegPair(reg1, reg2, spOffset);
        }
    }
}

//------------------------------------------------------------------------
// genPrologSaveReg: Like genPrologSaveRegPair, but for a single register. Save a single general-purpose or
// floating-point/SIMD register in a function or funclet prolog. Note that if we wish to change SP (i.e., spDelta != 0),
// then spOffset must be 8. This is because otherwise we would create an alignment hole above the saved register, not
// below it, which we currently don't support. This restriction could be loosened if the callers change to handle it
// (and this function changes to support using pre-indexed STR addressing). The caller must ensure that we can use the
// STR instruction, and that spOffset will be in the legal range for that instruction.
//
// Arguments:
//    reg1                     - Register to save.
//    spOffset                 - The offset from SP to store reg1 (must be positive or zero).
//    spDelta                  - If non-zero, the amount to add to SP before the register saves (must be negative or
//                               zero).
//    tmpReg                   - An available temporary register. Needed for the case of large frames.
//    pTmpRegIsZero            - If we use tmpReg, and pTmpRegIsZero is non-null, we set *pTmpRegIsZero to 'false'.
//                               Otherwise, we don't touch it.
//
// Return Value:
//    None.

void CodeGen::genPrologSaveReg(regNumber reg1, int spOffset, int spDelta, regNumber tmpReg, bool* pTmpRegIsZero)
{
    assert(spOffset >= 0);
    assert(spDelta <= 0);
    assert((spDelta % 16) == 0); // SP changes must be 16-byte aligned

    bool needToSaveRegs = true;
    if (spDelta != 0)
    {
        if ((spOffset == 0) && (spDelta >= -256))
        {
            // We can use pre-index addressing.
            // str REG, [SP, #spDelta]!
            getEmitter()->emitIns_R_R_I(INS_str, EA_PTRSIZE, reg1, REG_SPBASE, spDelta, INS_OPTS_PRE_INDEX);
            compiler->unwindSaveRegPreindexed(reg1, spDelta);

            needToSaveRegs = false;
        }
        else // (spOffset != 0) || (spDelta < -256)
        {
            // generate sub SP,SP,imm
            genStackPointerAdjustment(spDelta, tmpReg, pTmpRegIsZero, /* reportUnwindData */ true);
        }
    }

    if (needToSaveRegs)
    {
        // str REG, [SP, #offset]
        // 64-bit STR offset range: 0 to 32760, multiple of 8.
        getEmitter()->emitIns_R_R_I(INS_str, EA_PTRSIZE, reg1, REG_SPBASE, spOffset);
        compiler->unwindSaveReg(reg1, spOffset);
    }
}

//------------------------------------------------------------------------
// genEpilogRestoreRegPair: This is the opposite of genPrologSaveRegPair(), run in the epilog instead of the prolog.
// The stack pointer adjustment, if requested, is done after the register restore, using post-index addressing.
// The caller must ensure that we can use the LDP instruction, and that spOffset will be in the legal range for that
// instruction.
//
// Arguments:
//    reg1                     - First register of pair to restore.
//    reg2                     - Second register of pair to restore.
//    spOffset                 - The offset from SP to load reg1 (must be positive or zero).
//    spDelta                  - If non-zero, the amount to add to SP after the register restores (must be positive or
//                               zero).
//    useSaveNextPair          - True if the last prolog instruction was to save the previous register pair. This
//                               allows us to emit the "save_next" unwind code.
//    tmpReg                   - An available temporary register. Needed for the case of large frames.
//    pTmpRegIsZero            - If we use tmpReg, and pTmpRegIsZero is non-null, we set *pTmpRegIsZero to 'false'.
//                               Otherwise, we don't touch it.
//
// Return Value:
//    None.

void CodeGen::genEpilogRestoreRegPair(regNumber reg1,
                                      regNumber reg2,
                                      int       spOffset,
                                      int       spDelta,
                                      bool      useSaveNextPair,
                                      regNumber tmpReg,
                                      bool*     pTmpRegIsZero)
{
    assert(spOffset >= 0);
    assert(spDelta >= 0);
    assert((spDelta % 16) == 0);                                  // SP changes must be 16-byte aligned
    assert(genIsValidFloatReg(reg1) == genIsValidFloatReg(reg2)); // registers must be both general-purpose, or both
                                                                  // FP/SIMD

    if (spDelta != 0)
    {
        assert(!useSaveNextPair);
        if ((spOffset == 0) && (spDelta <= 504))
        {
            // Fold the SP change into this instruction.
            // ldp reg1, reg2, [SP], #spDelta
            getEmitter()->emitIns_R_R_R_I(INS_ldp, EA_PTRSIZE, reg1, reg2, REG_SPBASE, spDelta, INS_OPTS_POST_INDEX);
            compiler->unwindSaveRegPairPreindexed(reg1, reg2, -spDelta);
        }
        else // (spOffset != 0) || (spDelta > 504)
        {
            // Can't fold in the SP change; need to use a separate ADD instruction.

            // ldp reg1, reg2, [SP, #offset]
            getEmitter()->emitIns_R_R_R_I(INS_ldp, EA_PTRSIZE, reg1, reg2, REG_SPBASE, spOffset);
            compiler->unwindSaveRegPair(reg1, reg2, spOffset);

            // generate add SP,SP,imm
            genStackPointerAdjustment(spDelta, tmpReg, pTmpRegIsZero, /* reportUnwindData */ true);
        }
    }
    else
    {
        getEmitter()->emitIns_R_R_R_I(INS_ldp, EA_PTRSIZE, reg1, reg2, REG_SPBASE, spOffset);

        if (useSaveNextPair)
        {
            compiler->unwindSaveNext();
        }
        else
        {
            compiler->unwindSaveRegPair(reg1, reg2, spOffset);
        }
    }
}

//------------------------------------------------------------------------
// genEpilogRestoreReg: The opposite of genPrologSaveReg(), run in the epilog instead of the prolog.
//
// Arguments:
//    reg1                     - Register to restore.
//    spOffset                 - The offset from SP to restore reg1 (must be positive or zero).
//    spDelta                  - If non-zero, the amount to add to SP after the register restores (must be positive or
//                               zero).
//    tmpReg                   - An available temporary register. Needed for the case of large frames.
//    pTmpRegIsZero            - If we use tmpReg, and pTmpRegIsZero is non-null, we set *pTmpRegIsZero to 'false'.
//                               Otherwise, we don't touch it.
//
// Return Value:
//    None.

void CodeGen::genEpilogRestoreReg(regNumber reg1, int spOffset, int spDelta, regNumber tmpReg, bool* pTmpRegIsZero)
{
    assert(spOffset >= 0);
    assert(spDelta >= 0);
    assert((spDelta % 16) == 0); // SP changes must be 16-byte aligned

    if (spDelta != 0)
    {
        if ((spOffset == 0) && (spDelta <= 255))
        {
            // We can use post-index addressing.
            // ldr REG, [SP], #spDelta
            getEmitter()->emitIns_R_R_I(INS_ldr, EA_PTRSIZE, reg1, REG_SPBASE, spDelta, INS_OPTS_POST_INDEX);
            compiler->unwindSaveRegPreindexed(reg1, -spDelta);
        }
        else // (spOffset != 0) || (spDelta > 255)
        {
            // ldr reg1, [SP, #offset]
            getEmitter()->emitIns_R_R_I(INS_ldr, EA_PTRSIZE, reg1, REG_SPBASE, spOffset);
            compiler->unwindSaveReg(reg1, spOffset);

            // generate add SP,SP,imm
            genStackPointerAdjustment(spDelta, tmpReg, pTmpRegIsZero, /* reportUnwindData */ true);
        }
    }
    else
    {
        // ldr reg1, [SP, #offset]
        getEmitter()->emitIns_R_R_I(INS_ldr, EA_PTRSIZE, reg1, REG_SPBASE, spOffset);
        compiler->unwindSaveReg(reg1, spOffset);
    }
}

//------------------------------------------------------------------------
// genBuildRegPairsStack: Build a stack of register pairs for prolog/epilog save/restore for the given mask.
// The first register pair will contain the lowest register. Register pairs will combine neighbor
// registers in pairs. If it can't be done (for example if we have a hole or this is the last reg in a mask with
// odd number of regs) then the second element of that RegPair will be REG_NA.
//
// Arguments:
//   regsMask - a mask of registers for prolog/epilog generation;
//   regStack - a regStack instance to build the stack in, used to save temp copyings.
//
// Return value:
//   no return value; the regStack argument is modified.
//
// static
void CodeGen::genBuildRegPairsStack(regMaskTP regsMask, ArrayStack<RegPair>* regStack)
{
    assert(regStack != nullptr);
    assert(regStack->Height() == 0);

    unsigned regsCount = genCountBits(regsMask);

    while (regsMask != RBM_NONE)
    {
        regMaskTP reg1Mask = genFindLowestBit(regsMask);
        regNumber reg1     = genRegNumFromMask(reg1Mask);
        regsMask &= ~reg1Mask;
        regsCount -= 1;

        bool isPairSave = false;
        if (regsCount > 0)
        {
            regMaskTP reg2Mask = genFindLowestBit(regsMask);
            regNumber reg2     = genRegNumFromMask(reg2Mask);
            if (reg2 == REG_NEXT(reg1))
            {
                // The JIT doesn't allow saving pair (R28,FP), even though the
                // save_regp register pair unwind code specification allows it.
                // The JIT always saves (FP,LR) as a pair, and uses the save_fplr
                // unwind code. This only comes up in stress mode scenarios
                // where callee-saved registers are not allocated completely
                // from lowest-to-highest, without gaps.
                if (reg1 != REG_R28)
                {
                    // Both registers must have the same type to be saved as pair.
                    if (genIsValidFloatReg(reg1) == genIsValidFloatReg(reg2))
                    {
                        isPairSave = true;

                        regsMask &= ~reg2Mask;
                        regsCount -= 1;

                        regStack->Push(RegPair(reg1, reg2));
                    }
                }
            }
        }
        if (!isPairSave)
        {
            regStack->Push(RegPair(reg1));
        }
    }
    assert(regsCount == 0 && regsMask == RBM_NONE);

    genSetUseSaveNextPairs(regStack);
}

//------------------------------------------------------------------------
// genSetUseSaveNextPairs: Set useSaveNextPair for each RegPair on the stack which unwind info can be encoded as
// save_next code.
//
// Arguments:
//   regStack - a regStack instance to set useSaveNextPair.
//
// Notes:
// We can use save_next for RegPair(N, N+1) only when we have sequence like (N-2, N-1), (N, N+1).
// In this case in the prolog save_next for (N, N+1) refers to save_pair(N-2, N-1);
// in the epilog the unwinder will search for the first save_pair (N-2, N-1)
// and then go back to the first save_next (N, N+1) to restore it first.
//
// static
void CodeGen::genSetUseSaveNextPairs(ArrayStack<RegPair>* regStack)
{
    for (int i = 1; i < regStack->Height(); ++i)
    {
        RegPair& curr = regStack->BottomRef(i);
        RegPair  prev = regStack->Bottom(i - 1);

        if (prev.reg2 == REG_NA || curr.reg2 == REG_NA)
        {
            continue;
        }

        if (REG_NEXT(prev.reg2) != curr.reg1)
        {
            continue;
        }

        if (genIsValidFloatReg(prev.reg2) != genIsValidFloatReg(curr.reg1))
        {
            // It is possible to support changing of the last int pair with the first float pair,
            // but it is very rare case and it would require superfluous changes in the unwinder.
            continue;
        }
        curr.useSaveNextPair = true;
    }
}

//------------------------------------------------------------------------
// genGetSlotSizeForRegsInMask: Get the stack slot size appropriate for the register type from the mask.
//
// Arguments:
//   regsMask - a mask of registers for prolog/epilog generation.
//
// Return value:
//   stack slot size in bytes.
//
// Note: Because int and float register type sizes match we can call this function with a mask that includes both.
//
// static
int CodeGen::genGetSlotSizeForRegsInMask(regMaskTP regsMask)
{
    assert((regsMask & (RBM_CALLEE_SAVED | RBM_FP | RBM_LR)) == regsMask); // Do not expect anything else.

    static_assert_no_msg(REGSIZE_BYTES == FPSAVE_REGSIZE_BYTES);
    return REGSIZE_BYTES;
}

//------------------------------------------------------------------------
// genSaveCalleeSavedRegisterGroup: Saves the group of registers described by the mask.
//
// Arguments:
//   regsMask             - a mask of registers for prolog generation;
//   spDelta              - if non-zero, the amount to add to SP before the first register save (or together with it);
//   spOffset             - the offset from SP that is the beginning of the callee-saved register area;
//
void CodeGen::genSaveCalleeSavedRegisterGroup(regMaskTP regsMask, int spDelta, int spOffset)
{
    const int slotSize = genGetSlotSizeForRegsInMask(regsMask);

    ArrayStack<RegPair> regStack(compiler->getAllocator(CMK_Codegen));
    genBuildRegPairsStack(regsMask, &regStack);

    for (int i = 0; i < regStack.Height(); ++i)
    {
        RegPair regPair = regStack.Bottom(i);
        if (regPair.reg2 != REG_NA)
        {
            // We can use a STP instruction.
            genPrologSaveRegPair(regPair.reg1, regPair.reg2, spOffset, spDelta, regPair.useSaveNextPair, REG_IP0,
                                 nullptr);

            spOffset += 2 * slotSize;
        }
        else
        {
            // No register pair; we use a STR instruction.
            genPrologSaveReg(regPair.reg1, spOffset, spDelta, REG_IP0, nullptr);
            spOffset += slotSize;
        }

        spDelta = 0; // We've now changed SP already, if necessary; don't do it again.
    }
}

//------------------------------------------------------------------------
// genSaveCalleeSavedRegistersHelp: Save the callee-saved registers in 'regsToSaveMask' to the stack frame
// in the function or funclet prolog. Registers are saved in register number order from low addresses
// to high addresses. This means that integer registers are saved at lower addresses than floatint-point/SIMD
// registers. However, when genSaveFpLrWithAllCalleeSavedRegisters is true, the integer registers are stored
// at higher addresses than floating-point/SIMD registers, that is, the relative order of these two classes
// is reveresed. This is done to put the saved frame pointer very high in the frame, for simplicity.
//
// TODO: We could always put integer registers at the higher addresses, if desired, to remove this special
// case. It would cause many asm diffs when first implemented.
//
// If establishing frame pointer chaining, it must be done after saving the callee-saved registers.
//
// We can only use the instructions that are allowed by the unwind codes. The caller ensures that
// there is enough space on the frame to store these registers, and that the store instructions
// we need to use (STR or STP) are encodable with the stack-pointer immediate offsets we need to use.
//
// The caller can tell us to fold in a stack pointer adjustment, which we will do with the first instruction.
// Note that the stack pointer adjustment must be by a multiple of 16 to preserve the invariant that the
// stack pointer is always 16 byte aligned. If we are saving an odd number of callee-saved
// registers, though, we will have an empty aligment slot somewhere. It turns out we will put
// it below (at a lower address) the callee-saved registers, as that is currently how we
// do frame layout. This means that the first stack offset will be 8 and the stack pointer
// adjustment must be done by a SUB, and not folded in to a pre-indexed store.
//
// Arguments:
//    regsToSaveMask          - The mask of callee-saved registers to save. If empty, this function does nothing.
//    lowestCalleeSavedOffset - The offset from SP that is the beginning of the callee-saved register area. Note that
//                              if non-zero spDelta, then this is the offset of the first save *after* that
//                              SP adjustment.
//    spDelta                 - If non-zero, the amount to add to SP before the register saves (must be negative or
//                              zero).
//
// Notes:
//    The save set can contain LR in which case LR is saved along with the other callee-saved registers.
//    But currently Jit doesn't use frames without frame pointer on arm64.
//
void CodeGen::genSaveCalleeSavedRegistersHelp(regMaskTP regsToSaveMask, int lowestCalleeSavedOffset, int spDelta)
{
    assert(spDelta <= 0);
    unsigned regsToSaveCount = genCountBits(regsToSaveMask);
    if (regsToSaveCount == 0)
    {
        if (spDelta != 0)
        {
            // Currently this is the case for varargs only
            // whose size is MAX_REG_ARG * REGSIZE_BYTES = 64 bytes.
            genStackPointerAdjustment(spDelta, REG_NA, nullptr, /* reportUnwindData */ true);
        }
        return;
    }

    assert((spDelta % 16) == 0);

    // We also can save FP and LR, even though they are not in RBM_CALLEE_SAVED.
    assert(regsToSaveCount <= genCountBits(RBM_CALLEE_SAVED | RBM_FP | RBM_LR));

    // Save integer registers at higher addresses than floating-point registers.

    regMaskTP maskSaveRegsFloat = regsToSaveMask & RBM_ALLFLOAT;
    regMaskTP maskSaveRegsInt   = regsToSaveMask & ~maskSaveRegsFloat;

    if (maskSaveRegsFloat != RBM_NONE)
    {
        genSaveCalleeSavedRegisterGroup(maskSaveRegsFloat, spDelta, lowestCalleeSavedOffset);
        spDelta = 0;
        lowestCalleeSavedOffset += genCountBits(maskSaveRegsFloat) * FPSAVE_REGSIZE_BYTES;
    }

    if (maskSaveRegsInt != RBM_NONE)
    {
        genSaveCalleeSavedRegisterGroup(maskSaveRegsInt, spDelta, lowestCalleeSavedOffset);
        // No need to update spDelta, lowestCalleeSavedOffset since they're not used after this.
    }
}

//------------------------------------------------------------------------
// genRestoreCalleeSavedRegisterGroup: Restores the group of registers described by the mask.
//
// Arguments:
//   regsMask             - a mask of registers for epilog generation;
//   spDelta              - if non-zero, the amount to add to SP after the last register restore (or together with it);
//   spOffset             - the offset from SP that is the beginning of the callee-saved register area;
//
void CodeGen::genRestoreCalleeSavedRegisterGroup(regMaskTP regsMask, int spDelta, int spOffset)
{
    const int slotSize = genGetSlotSizeForRegsInMask(regsMask);

    ArrayStack<RegPair> regStack(compiler->getAllocator(CMK_Codegen));
    genBuildRegPairsStack(regsMask, &regStack);

    int stackDelta = 0;
    for (int i = 0; i < regStack.Height(); ++i)
    {
        bool lastRestoreInTheGroup = (i == regStack.Height() - 1);
        bool updateStackDelta      = lastRestoreInTheGroup && (spDelta != 0);
        if (updateStackDelta)
        {
            // Update stack delta only if it is the last restore (the first save).
            assert(stackDelta == 0);
            stackDelta = spDelta;
        }

        RegPair regPair = regStack.Index(i);
        if (regPair.reg2 != REG_NA)
        {
            spOffset -= 2 * slotSize;

            genEpilogRestoreRegPair(regPair.reg1, regPair.reg2, spOffset, stackDelta, regPair.useSaveNextPair, REG_IP1,
                                    nullptr);
        }
        else
        {
            spOffset -= slotSize;
            genEpilogRestoreReg(regPair.reg1, spOffset, stackDelta, REG_IP1, nullptr);
        }
    }
}

//------------------------------------------------------------------------
// genRestoreCalleeSavedRegistersHelp: Restore the callee-saved registers in 'regsToRestoreMask' from the stack frame
// in the function or funclet epilog. This exactly reverses the actions of genSaveCalleeSavedRegistersHelp().
//
// Arguments:
//    regsToRestoreMask       - The mask of callee-saved registers to restore. If empty, this function does nothing.
//    lowestCalleeSavedOffset - The offset from SP that is the beginning of the callee-saved register area.
//    spDelta                 - If non-zero, the amount to add to SP after the register restores (must be positive or
//                              zero).
//
// Here's an example restore sequence:
//      ldp     x27, x28, [sp,#96]
//      ldp     x25, x26, [sp,#80]
//      ldp     x23, x24, [sp,#64]
//      ldp     x21, x22, [sp,#48]
//      ldp     x19, x20, [sp,#32]
//
// For the case of non-zero spDelta, we assume the base of the callee-save registers to restore is at SP, and
// the last restore adjusts SP by the specified amount. For example:
//      ldp     x27, x28, [sp,#64]
//      ldp     x25, x26, [sp,#48]
//      ldp     x23, x24, [sp,#32]
//      ldp     x21, x22, [sp,#16]
//      ldp     x19, x20, [sp], #80
//
// Note you call the unwind functions specifying the prolog operation that is being un-done. So, for example, when
// generating a post-indexed load, you call the unwind function for specifying the corresponding preindexed store.
//
// Return Value:
//    None.

void CodeGen::genRestoreCalleeSavedRegistersHelp(regMaskTP regsToRestoreMask, int lowestCalleeSavedOffset, int spDelta)
{
    assert(spDelta >= 0);
    unsigned regsToRestoreCount = genCountBits(regsToRestoreMask);
    if (regsToRestoreCount == 0)
    {
        if (spDelta != 0)
        {
            // Currently this is the case for varargs only
            // whose size is MAX_REG_ARG * REGSIZE_BYTES = 64 bytes.
            genStackPointerAdjustment(spDelta, REG_NA, nullptr, /* reportUnwindData */ true);
        }
        return;
    }

    assert((spDelta % 16) == 0);

    // We also can restore FP and LR, even though they are not in RBM_CALLEE_SAVED.
    assert(regsToRestoreCount <= genCountBits(RBM_CALLEE_SAVED | RBM_FP | RBM_LR));

    // Point past the end, to start. We predecrement to find the offset to load from.
    static_assert_no_msg(REGSIZE_BYTES == FPSAVE_REGSIZE_BYTES);
    int spOffset = lowestCalleeSavedOffset + regsToRestoreCount * REGSIZE_BYTES;

    // Save integer registers at higher addresses than floating-point registers.

    regMaskTP maskRestoreRegsFloat = regsToRestoreMask & RBM_ALLFLOAT;
    regMaskTP maskRestoreRegsInt   = regsToRestoreMask & ~maskRestoreRegsFloat;

    // Restore in the opposite order of saving.

    if (maskRestoreRegsInt != RBM_NONE)
    {
        int spIntDelta = (maskRestoreRegsFloat != RBM_NONE) ? 0 : spDelta; // should we delay the SP adjustment?
        genRestoreCalleeSavedRegisterGroup(maskRestoreRegsInt, spIntDelta, spOffset);
        spOffset -= genCountBits(maskRestoreRegsInt) * REGSIZE_BYTES;
    }

    if (maskRestoreRegsFloat != RBM_NONE)
    {
        // If there is any spDelta, it must be used here.
        genRestoreCalleeSavedRegisterGroup(maskRestoreRegsFloat, spDelta, spOffset);
        // No need to update spOffset since it's not used after this.
    }
}

// clang-format off
/*****************************************************************************
 *
 *  Generates code for an EH funclet prolog.
 *
 *  Funclets have the following incoming arguments:
 *
 *      catch:          x0 = the exception object that was caught (see GT_CATCH_ARG)
 *      filter:         x0 = the exception object to filter (see GT_CATCH_ARG), x1 = CallerSP of the containing function
 *      finally/fault:  none
 *
 *  Funclets set the following registers on exit:
 *
 *      catch:          x0 = the address at which execution should resume (see BBJ_EHCATCHRET)
 *      filter:         x0 = non-zero if the handler should handle the exception, zero otherwise (see GT_RETFILT)
 *      finally/fault:  none
 *
 *  The ARM64 funclet prolog sequence is one of the following (Note: #framesz is total funclet frame size,
 *  including everything; #outsz is outgoing argument space. #framesz must be a multiple of 16):
 *
 *  Frame type 1:
 *     For #outsz == 0 and #framesz <= 512:
 *     stp fp,lr,[sp,-#framesz]!    ; establish the frame (predecrement by #framesz), save FP/LR
 *     stp x19,x20,[sp,#xxx]        ; save callee-saved registers, as necessary
 *
 *  The funclet frame is thus:
 *
 *      |                       |
 *      |-----------------------|
 *      |  incoming arguments   |
 *      +=======================+ <---- Caller's SP
 *      |  Varargs regs space   | // Only for varargs main functions; 64 bytes
 *      |-----------------------|
 *      |Callee saved registers | // multiple of 8 bytes
 *      |-----------------------|
 *      |        PSP slot       | // 8 bytes (omitted in CoreRT ABI)
 *      |-----------------------|
 *      ~  alignment padding    ~ // To make the whole frame 16 byte aligned.
 *      |-----------------------|
 *      |      Saved FP, LR     | // 16 bytes
 *      |-----------------------| <---- Ambient SP
 *      |       |               |
 *      ~       | Stack grows   ~
 *      |       | downward      |
 *              V
 *
 *  Frame type 2:
 *     For #outsz != 0 and #framesz <= 512:
 *     sub sp,sp,#framesz           ; establish the frame
 *     stp fp,lr,[sp,#outsz]        ; save FP/LR.
 *     stp x19,x20,[sp,#xxx]        ; save callee-saved registers, as necessary
 *
 *  The funclet frame is thus:
 *
 *      |                       |
 *      |-----------------------|
 *      |  incoming arguments   |
 *      +=======================+ <---- Caller's SP
 *      |  Varargs regs space   | // Only for varargs main functions; 64 bytes
 *      |-----------------------|
 *      |Callee saved registers | // multiple of 8 bytes
 *      |-----------------------|
 *      |        PSP slot       | // 8 bytes (omitted in CoreRT ABI)
 *      |-----------------------|
 *      ~  alignment padding    ~ // To make the whole frame 16 byte aligned.
 *      |-----------------------|
 *      |      Saved FP, LR     | // 16 bytes
 *      |-----------------------|
 *      |   Outgoing arg space  | // multiple of 8 bytes
 *      |-----------------------| <---- Ambient SP
 *      |       |               |
 *      ~       | Stack grows   ~
 *      |       | downward      |
 *              V
 *
 *  Frame type 3:
 *     For #framesz > 512:
 *     stp fp,lr,[sp,- (#framesz - #outsz)]!    ; establish the frame, save FP/LR
 *                                              ; note that it is guaranteed here that (#framesz - #outsz) <= 240
 *     stp x19,x20,[sp,#xxx]                    ; save callee-saved registers, as necessary
 *     sub sp,sp,#outsz                         ; create space for outgoing argument space
 *
 *  The funclet frame is thus:
 *
 *      |                       |
 *      |-----------------------|
 *      |  incoming arguments   |
 *      +=======================+ <---- Caller's SP
 *      |  Varargs regs space   | // Only for varargs main functions; 64 bytes
 *      |-----------------------|
 *      |Callee saved registers | // multiple of 8 bytes
 *      |-----------------------|
 *      |        PSP slot       | // 8 bytes (omitted in CoreRT ABI)
 *      |-----------------------|
 *      ~  alignment padding    ~ // To make the first SP subtraction 16 byte aligned
 *      |-----------------------|
 *      |      Saved FP, LR     | // 16 bytes
 *      |-----------------------|
 *      ~  alignment padding    ~ // To make the whole frame 16 byte aligned (specifically, to 16-byte align the outgoing argument space).
 *      |-----------------------|
 *      |   Outgoing arg space  | // multiple of 8 bytes
 *      |-----------------------| <---- Ambient SP
 *      |       |               |
 *      ~       | Stack grows   ~
 *      |       | downward      |
 *              V
 *
 * Both #1 and #2 only change SP once. That means that there will be a maximum of one alignment slot needed. For the general case, #3,
 * it is possible that we will need to add alignment to both changes to SP, leading to 16 bytes of alignment. Remember that the stack
 * pointer needs to be 16 byte aligned at all times. The size of the PSP slot plus callee-saved registers space is a maximum of 240 bytes:
 *
 *     FP,LR registers
 *     10 int callee-saved register x19-x28
 *     8 float callee-saved registers v8-v15
 *     8 saved integer argument registers x0-x7, if varargs function
 *     1 PSP slot
 *     1 alignment slot
 *     == 30 slots * 8 bytes = 240 bytes.
 *
 * The outgoing argument size, however, can be very large, if we call a function that takes a large number of
 * arguments (note that we currently use the same outgoing argument space size in the funclet as for the main
 * function, even if the funclet doesn't have any calls, or has a much smaller, or larger, maximum number of
 * outgoing arguments for any call). In that case, we need to 16-byte align the initial change to SP, before
 * saving off the callee-saved registers and establishing the PSPsym, so we can use the limited immediate offset
 * encodings we have available, before doing another 16-byte aligned SP adjustment to create the outgoing argument
 * space. Both changes to SP might need to add alignment padding.
 *
 * In addition to the above "standard" frames, we also need to support a frame where the saved FP/LR are at the
 * highest addresses. This is to match the frame layout (specifically, callee-saved registers including FP/LR
 * and the PSPSym) that is used in the main function when a GS cookie is required due to the use of localloc.
 * (Note that localloc cannot be used in a funclet.) In these variants, not only has the position of FP/LR
 * changed, but where the alignment padding is placed has also changed.
 *
 *  Frame type 4 (variant of frame types 1 and 2):
 *     For #framesz <= 512:
 *     sub sp,sp,#framesz           ; establish the frame
 *     stp x19,x20,[sp,#xxx]        ; save callee-saved registers, as necessary
 *     stp fp,lr,[sp,#yyy]          ; save FP/LR.
 *     ; write PSPSym
 *
 *  The "#framesz <= 512" condition ensures that after we've established the frame, we can use "stp" with its
 *  maximum allowed offset (504) to save the callee-saved register at the highest address.
 *
 *  We use "sub" instead of folding it into the next instruction as a predecrement, as we need to write PSPSym
 *  at the bottom of the stack, and there might also be an alignment padding slot.
 *
 *  The funclet frame is thus:
 *
 *      |                       |
 *      |-----------------------|
 *      |  incoming arguments   |
 *      +=======================+ <---- Caller's SP
 *      |  Varargs regs space   | // Only for varargs main functions; 64 bytes
 *      |-----------------------|
 *      |      Saved LR         | // 8 bytes
 *      |-----------------------|
 *      |      Saved FP         | // 8 bytes
 *      |-----------------------|
 *      |Callee saved registers | // multiple of 8 bytes
 *      |-----------------------|
 *      |        PSP slot       | // 8 bytes (omitted in CoreRT ABI)
 *      |-----------------------|
 *      ~  alignment padding    ~ // To make the whole frame 16 byte aligned.
 *      |-----------------------|
 *      |   Outgoing arg space  | // multiple of 8 bytes (optional; if #outsz > 0)
 *      |-----------------------| <---- Ambient SP
 *      |       |               |
 *      ~       | Stack grows   ~
 *      |       | downward      |
 *              V
 *
 *  Frame type 5 (variant of frame type 3):
 *     For #framesz > 512:
 *     sub sp,sp,(#framesz - #outsz) ; establish part of the frame. Note that it is guaranteed here that (#framesz - #outsz) <= 240
 *     stp x19,x20,[sp,#xxx]        ; save callee-saved registers, as necessary
 *     stp fp,lr,[sp,#yyy]          ; save FP/LR.
 *     sub sp,sp,#outsz             ; create space for outgoing argument space
 *     ; write PSPSym
 *
 *  For large frames with "#framesz > 512", we must do one SP adjustment first, after which we can save callee-saved
 *  registers with up to the maximum "stp" offset of 504. Then, we can establish the rest of the frame (namely, the
 *  space for the outgoing argument space).
 *
 *  The funclet frame is thus:
 *
 *      |                       |
 *      |-----------------------|
 *      |  incoming arguments   |
 *      +=======================+ <---- Caller's SP
 *      |  Varargs regs space   | // Only for varargs main functions; 64 bytes
 *      |-----------------------|
 *      |      Saved LR         | // 8 bytes
 *      |-----------------------|
 *      |      Saved FP         | // 8 bytes
 *      |-----------------------|
 *      |Callee saved registers | // multiple of 8 bytes
 *      |-----------------------|
 *      |        PSP slot       | // 8 bytes (omitted in CoreRT ABI)
 *      |-----------------------|
 *      ~  alignment padding    ~ // To make the first SP subtraction 16 byte aligned
 *      |-----------------------|
 *      ~  alignment padding    ~ // To make the whole frame 16 byte aligned (specifically, to 16-byte align the outgoing argument space).
 *      |-----------------------|
 *      |   Outgoing arg space  | // multiple of 8 bytes
 *      |-----------------------| <---- Ambient SP
 *      |       |               |
 *      ~       | Stack grows   ~
 *      |       | downward      |
 *              V
 *
 * Note that in this case we might have 16 bytes of alignment that is adjacent. This is because we are doing 2 SP
 * subtractions, and each one must be aligned up to 16 bytes.
 *
 * Note that in all cases, the PSPSym is in exactly the same position with respect to Caller-SP, and that location is the same relative to Caller-SP
 * as in the main function.
 *
 * Funclets do not have varargs arguments. However, because the PSPSym must exist at the same offset from Caller-SP as in the main function, we
 * must add buffer space for the saved varargs argument registers here, if the main function did the same.
 *
 *     ; After this header, fill the PSP slot, for use by the VM (it gets reported with the GC info), or by code generation of nested filters.
 *     ; This is not part of the "OS prolog"; it has no associated unwind data, and is not reversed in the funclet epilog.
 *
 *     if (this is a filter funclet)
 *     {
 *          // x1 on entry to a filter funclet is CallerSP of the containing function:
 *          // either the main function, or the funclet for a handler that this filter is dynamically nested within.
 *          // Note that a filter can be dynamically nested within a funclet even if it is not statically within
 *          // a funclet. Consider:
 *          //
 *          //    try {
 *          //        try {
 *          //            throw new Exception();
 *          //        } catch(Exception) {
 *          //            throw new Exception();     // The exception thrown here ...
 *          //        }
 *          //    } filter {                         // ... will be processed here, while the "catch" funclet frame is still on the stack
 *          //    } filter-handler {
 *          //    }
 *          //
 *          // Because of this, we need a PSP in the main function anytime a filter funclet doesn't know whether the enclosing frame will
 *          // be a funclet or main function. We won't know any time there is a filter protecting nested EH. To simplify, we just always
 *          // create a main function PSP for any function with a filter.
 *
 *          ldr x1, [x1, #CallerSP_to_PSP_slot_delta]  ; Load the CallerSP of the main function (stored in the PSP of the dynamically containing funclet or function)
 *          str x1, [sp, #SP_to_PSP_slot_delta]        ; store the PSP
 *          add fp, x1, #Function_CallerSP_to_FP_delta ; re-establish the frame pointer
 *     }
 *     else
 *     {
 *          // This is NOT a filter funclet. The VM re-establishes the frame pointer on entry.
 *          // TODO-ARM64-CQ: if VM set x1 to CallerSP on entry, like for filters, we could save an instruction.
 *
 *          add x3, fp, #Function_FP_to_CallerSP_delta  ; compute the CallerSP, given the frame pointer. x3 is scratch.
 *          str x3, [sp, #SP_to_PSP_slot_delta]         ; store the PSP
 *     }
 *
 *  An example epilog sequence is then:
 *
 *     add sp,sp,#outsz             ; if any outgoing argument space
 *     ...                          ; restore callee-saved registers
 *     ldp x19,x20,[sp,#xxx]
 *     ldp fp,lr,[sp],#framesz
 *     ret lr
 *
 */
// clang-format on

void CodeGen::genFuncletProlog(BasicBlock* block)
{
#ifdef DEBUG
    if (verbose)
        printf("*************** In genFuncletProlog()\n");
#endif

    assert(block != NULL);
    assert(block->bbFlags & BBF_FUNCLET_BEG);

    ScopedSetVariable<bool> _setGeneratingProlog(&compiler->compGeneratingProlog, true);

    gcInfo.gcResetForBB();

    compiler->unwindBegProlog();

    regMaskTP maskSaveRegsFloat = genFuncletInfo.fiSaveRegs & RBM_ALLFLOAT;
    regMaskTP maskSaveRegsInt   = genFuncletInfo.fiSaveRegs & ~maskSaveRegsFloat;

    // Funclets must always save LR and FP, since when we have funclets we must have an FP frame.
    assert((maskSaveRegsInt & RBM_LR) != 0);
    assert((maskSaveRegsInt & RBM_FP) != 0);

    bool isFilter = (block->bbCatchTyp == BBCT_FILTER);

    regMaskTP maskArgRegsLiveIn;
    if (isFilter)
    {
        maskArgRegsLiveIn = RBM_R0 | RBM_R1;
    }
    else if ((block->bbCatchTyp == BBCT_FINALLY) || (block->bbCatchTyp == BBCT_FAULT))
    {
        maskArgRegsLiveIn = RBM_NONE;
    }
    else
    {
        maskArgRegsLiveIn = RBM_R0;
    }

    if (genFuncletInfo.fiFrameType == 1)
    {
        getEmitter()->emitIns_R_R_R_I(INS_stp, EA_PTRSIZE, REG_FP, REG_LR, REG_SPBASE, genFuncletInfo.fiSpDelta1,
                                      INS_OPTS_PRE_INDEX);
        compiler->unwindSaveRegPairPreindexed(REG_FP, REG_LR, genFuncletInfo.fiSpDelta1);

        maskSaveRegsInt &= ~(RBM_LR | RBM_FP); // We've saved these now

        assert(genFuncletInfo.fiSpDelta2 == 0);
        assert(genFuncletInfo.fiSP_to_FPLR_save_delta == 0);
    }
    else if (genFuncletInfo.fiFrameType == 2)
    {
        // fiFrameType==2 constraints:
        assert(genFuncletInfo.fiSpDelta1 < 0);
        assert(genFuncletInfo.fiSpDelta1 >= -512);

        // generate sub SP,SP,imm
        genStackPointerAdjustment(genFuncletInfo.fiSpDelta1, REG_NA, nullptr, /* reportUnwindData */ true);

        assert(genFuncletInfo.fiSpDelta2 == 0);

        getEmitter()->emitIns_R_R_R_I(INS_stp, EA_PTRSIZE, REG_FP, REG_LR, REG_SPBASE,
                                      genFuncletInfo.fiSP_to_FPLR_save_delta);
        compiler->unwindSaveRegPair(REG_FP, REG_LR, genFuncletInfo.fiSP_to_FPLR_save_delta);

        maskSaveRegsInt &= ~(RBM_LR | RBM_FP); // We've saved these now
    }
    else if (genFuncletInfo.fiFrameType == 3)
    {
        getEmitter()->emitIns_R_R_R_I(INS_stp, EA_PTRSIZE, REG_FP, REG_LR, REG_SPBASE, genFuncletInfo.fiSpDelta1,
                                      INS_OPTS_PRE_INDEX);
        compiler->unwindSaveRegPairPreindexed(REG_FP, REG_LR, genFuncletInfo.fiSpDelta1);

        maskSaveRegsInt &= ~(RBM_LR | RBM_FP); // We've saved these now
    }
    else if (genFuncletInfo.fiFrameType == 4)
    {
        // fiFrameType==4 constraints:
        assert(genFuncletInfo.fiSpDelta1 < 0);
        assert(genFuncletInfo.fiSpDelta1 >= -512);

        // generate sub SP,SP,imm
        genStackPointerAdjustment(genFuncletInfo.fiSpDelta1, REG_NA, nullptr, /* reportUnwindData */ true);

        assert(genFuncletInfo.fiSpDelta2 == 0);
    }
    else
    {
        assert(genFuncletInfo.fiFrameType == 5);

        // Nothing to do here; the first SP adjustment will be done by saving the callee-saved registers.
    }

    int lowestCalleeSavedOffset = genFuncletInfo.fiSP_to_CalleeSave_delta +
                                  genFuncletInfo.fiSpDelta2; // We haven't done the second adjustment of SP yet (if any)
    genSaveCalleeSavedRegistersHelp(maskSaveRegsInt | maskSaveRegsFloat, lowestCalleeSavedOffset, 0);

    if ((genFuncletInfo.fiFrameType == 3) || (genFuncletInfo.fiFrameType == 5))
    {
        // Note that genFuncletInfo.fiSpDelta2 is always a negative value
        assert(genFuncletInfo.fiSpDelta2 < 0);

        // generate sub SP,SP,imm
        genStackPointerAdjustment(genFuncletInfo.fiSpDelta2, REG_R2, nullptr, /* reportUnwindData */ true);
    }

    // This is the end of the OS-reported prolog for purposes of unwinding
    compiler->unwindEndProlog();

    // If there is no PSPSym (CoreRT ABI), we are done. Otherwise, we need to set up the PSPSym in the functlet frame.
    if (compiler->lvaPSPSym != BAD_VAR_NUM)
    {
        if (isFilter)
        {
            // This is the first block of a filter
            // Note that register x1 = CallerSP of the containing function
            // X1 is overwritten by the first Load (new callerSP)
            // X2 is scratch when we have a large constant offset

            // Load the CallerSP of the main function (stored in the PSP of the dynamically containing funclet or
            // function)
            genInstrWithConstant(ins_Load(TYP_I_IMPL), EA_PTRSIZE, REG_R1, REG_R1,
                                 genFuncletInfo.fiCallerSP_to_PSP_slot_delta, REG_R2, false);
            regSet.verifyRegUsed(REG_R1);

            // Store the PSP value (aka CallerSP)
            genInstrWithConstant(ins_Store(TYP_I_IMPL), EA_PTRSIZE, REG_R1, REG_SPBASE,
                                 genFuncletInfo.fiSP_to_PSP_slot_delta, REG_R2, false);

            // re-establish the frame pointer
            genInstrWithConstant(INS_add, EA_PTRSIZE, REG_FPBASE, REG_R1,
                                 genFuncletInfo.fiFunction_CallerSP_to_FP_delta, REG_R2, false);
        }
        else // This is a non-filter funclet
        {
            // X3 is scratch, X2 can also become scratch

            // compute the CallerSP, given the frame pointer. x3 is scratch.
            genInstrWithConstant(INS_add, EA_PTRSIZE, REG_R3, REG_FPBASE,
                                 -genFuncletInfo.fiFunction_CallerSP_to_FP_delta, REG_R2, false);
            regSet.verifyRegUsed(REG_R3);

            genInstrWithConstant(ins_Store(TYP_I_IMPL), EA_PTRSIZE, REG_R3, REG_SPBASE,
                                 genFuncletInfo.fiSP_to_PSP_slot_delta, REG_R2, false);
        }
    }
}

/*****************************************************************************
 *
 *  Generates code for an EH funclet epilog.
 */

void CodeGen::genFuncletEpilog()
{
#ifdef DEBUG
    if (verbose)
        printf("*************** In genFuncletEpilog()\n");
#endif

    ScopedSetVariable<bool> _setGeneratingEpilog(&compiler->compGeneratingEpilog, true);

    bool unwindStarted = false;

    if (!unwindStarted)
    {
        // We can delay this until we know we'll generate an unwindable instruction, if necessary.
        compiler->unwindBegEpilog();
        unwindStarted = true;
    }

    regMaskTP maskRestoreRegsFloat = genFuncletInfo.fiSaveRegs & RBM_ALLFLOAT;
    regMaskTP maskRestoreRegsInt   = genFuncletInfo.fiSaveRegs & ~maskRestoreRegsFloat;

    // Funclets must always save LR and FP, since when we have funclets we must have an FP frame.
    assert((maskRestoreRegsInt & RBM_LR) != 0);
    assert((maskRestoreRegsInt & RBM_FP) != 0);

    if ((genFuncletInfo.fiFrameType == 3) || (genFuncletInfo.fiFrameType == 5))
    {
        // Note that genFuncletInfo.fiSpDelta2 is always a negative value
        assert(genFuncletInfo.fiSpDelta2 < 0);

        // generate add SP,SP,imm
        genStackPointerAdjustment(-genFuncletInfo.fiSpDelta2, REG_R2, nullptr, /* reportUnwindData */ true);
    }

    regMaskTP regsToRestoreMask = maskRestoreRegsInt | maskRestoreRegsFloat;
    if ((genFuncletInfo.fiFrameType == 1) || (genFuncletInfo.fiFrameType == 2) || (genFuncletInfo.fiFrameType == 3))
    {
        regsToRestoreMask &= ~(RBM_LR | RBM_FP); // We restore FP/LR at the end
    }
    int lowestCalleeSavedOffset = genFuncletInfo.fiSP_to_CalleeSave_delta + genFuncletInfo.fiSpDelta2;
    genRestoreCalleeSavedRegistersHelp(regsToRestoreMask, lowestCalleeSavedOffset, 0);

    if (genFuncletInfo.fiFrameType == 1)
    {
        getEmitter()->emitIns_R_R_R_I(INS_ldp, EA_PTRSIZE, REG_FP, REG_LR, REG_SPBASE, -genFuncletInfo.fiSpDelta1,
                                      INS_OPTS_POST_INDEX);
        compiler->unwindSaveRegPairPreindexed(REG_FP, REG_LR, genFuncletInfo.fiSpDelta1);

        assert(genFuncletInfo.fiSpDelta2 == 0);
        assert(genFuncletInfo.fiSP_to_FPLR_save_delta == 0);
    }
    else if (genFuncletInfo.fiFrameType == 2)
    {
        getEmitter()->emitIns_R_R_R_I(INS_ldp, EA_PTRSIZE, REG_FP, REG_LR, REG_SPBASE,
                                      genFuncletInfo.fiSP_to_FPLR_save_delta);
        compiler->unwindSaveRegPair(REG_FP, REG_LR, genFuncletInfo.fiSP_to_FPLR_save_delta);

        // fiFrameType==2 constraints:
        assert(genFuncletInfo.fiSpDelta1 < 0);
        assert(genFuncletInfo.fiSpDelta1 >= -512);

        // generate add SP,SP,imm
        genStackPointerAdjustment(-genFuncletInfo.fiSpDelta1, REG_NA, nullptr, /* reportUnwindData */ true);

        assert(genFuncletInfo.fiSpDelta2 == 0);
    }
    else if (genFuncletInfo.fiFrameType == 3)
    {
        getEmitter()->emitIns_R_R_R_I(INS_ldp, EA_PTRSIZE, REG_FP, REG_LR, REG_SPBASE, -genFuncletInfo.fiSpDelta1,
                                      INS_OPTS_POST_INDEX);
        compiler->unwindSaveRegPairPreindexed(REG_FP, REG_LR, genFuncletInfo.fiSpDelta1);
    }
    else if (genFuncletInfo.fiFrameType == 4)
    {
        // fiFrameType==4 constraints:
        assert(genFuncletInfo.fiSpDelta1 < 0);
        assert(genFuncletInfo.fiSpDelta1 >= -512);

        // generate add SP,SP,imm
        genStackPointerAdjustment(-genFuncletInfo.fiSpDelta1, REG_NA, nullptr, /* reportUnwindData */ true);

        assert(genFuncletInfo.fiSpDelta2 == 0);
    }
    else
    {
        assert(genFuncletInfo.fiFrameType == 5);
        // Same work as fiFrameType==4, but different asserts.

        assert(genFuncletInfo.fiSpDelta1 < 0);
        assert(genFuncletInfo.fiSpDelta1 >= -240);

        // generate add SP,SP,imm
        genStackPointerAdjustment(-genFuncletInfo.fiSpDelta1, REG_NA, nullptr, /* reportUnwindData */ true);
    }

    inst_RV(INS_ret, REG_LR, TYP_I_IMPL);
    compiler->unwindReturn(REG_LR);

    compiler->unwindEndEpilog();
}

/*****************************************************************************
 *
 *  Capture the information used to generate the funclet prologs and epilogs.
 *  Note that all funclet prologs are identical, and all funclet epilogs are
 *  identical (per type: filters are identical, and non-filters are identical).
 *  Thus, we compute the data used for these just once.
 *
 *  See genFuncletProlog() for more information about the prolog/epilog sequences.
 */

void CodeGen::genCaptureFuncletPrologEpilogInfo()
{
    if (!compiler->ehAnyFunclets())
        return;

    assert(isFramePointerUsed());

    // The frame size and offsets must be finalized
    assert(compiler->lvaDoneFrameLayout == Compiler::FINAL_FRAME_LAYOUT);

    genFuncletInfo.fiFunction_CallerSP_to_FP_delta = genCallerSPtoFPdelta();

    regMaskTP rsMaskSaveRegs = regSet.rsMaskCalleeSaved;
    assert((rsMaskSaveRegs & RBM_LR) != 0);
    assert((rsMaskSaveRegs & RBM_FP) != 0);

    unsigned PSPSize = (compiler->lvaPSPSym != BAD_VAR_NUM) ? REGSIZE_BYTES : 0;

    unsigned saveRegsCount       = genCountBits(rsMaskSaveRegs);
    unsigned saveRegsPlusPSPSize = saveRegsCount * REGSIZE_BYTES + PSPSize;
    if (compiler->info.compIsVarArgs)
    {
        // For varargs we always save all of the integer register arguments
        // so that they are contiguous with the incoming stack arguments.
        saveRegsPlusPSPSize += MAX_REG_ARG * REGSIZE_BYTES;
    }
    unsigned saveRegsPlusPSPSizeAligned = roundUp(saveRegsPlusPSPSize, STACK_ALIGN);

    assert(compiler->lvaOutgoingArgSpaceSize % REGSIZE_BYTES == 0);
    unsigned outgoingArgSpaceAligned = roundUp(compiler->lvaOutgoingArgSpaceSize, STACK_ALIGN);

    unsigned maxFuncletFrameSizeAligned = saveRegsPlusPSPSizeAligned + outgoingArgSpaceAligned;
    assert((maxFuncletFrameSizeAligned % STACK_ALIGN) == 0);

    int SP_to_FPLR_save_delta;
    int SP_to_PSP_slot_delta;
    int CallerSP_to_PSP_slot_delta;

    unsigned funcletFrameSize        = saveRegsPlusPSPSize + compiler->lvaOutgoingArgSpaceSize;
    unsigned funcletFrameSizeAligned = roundUp(funcletFrameSize, STACK_ALIGN);
    assert(funcletFrameSizeAligned <= maxFuncletFrameSizeAligned);

    unsigned funcletFrameAlignmentPad = funcletFrameSizeAligned - funcletFrameSize;
    assert((funcletFrameAlignmentPad == 0) || (funcletFrameAlignmentPad == REGSIZE_BYTES));

    if (maxFuncletFrameSizeAligned <= 512)
    {
        if (genSaveFpLrWithAllCalleeSavedRegisters)
        {
            SP_to_FPLR_save_delta = funcletFrameSizeAligned - (2 /* FP, LR */ * REGSIZE_BYTES);
            if (compiler->info.compIsVarArgs)
            {
                SP_to_FPLR_save_delta -= MAX_REG_ARG * REGSIZE_BYTES;
            }

            SP_to_PSP_slot_delta       = compiler->lvaOutgoingArgSpaceSize + funcletFrameAlignmentPad;
            CallerSP_to_PSP_slot_delta = -(int)saveRegsPlusPSPSize;

            genFuncletInfo.fiFrameType = 4;
        }
        else
        {
            SP_to_FPLR_save_delta = compiler->lvaOutgoingArgSpaceSize;
            SP_to_PSP_slot_delta  = SP_to_FPLR_save_delta + 2 /* FP, LR */ * REGSIZE_BYTES + funcletFrameAlignmentPad;
            CallerSP_to_PSP_slot_delta = -(int)(saveRegsPlusPSPSize - 2 /* FP, LR */ * REGSIZE_BYTES);

            if (compiler->lvaOutgoingArgSpaceSize == 0)
            {
                genFuncletInfo.fiFrameType = 1;
            }
            else
            {
                genFuncletInfo.fiFrameType = 2;
            }
        }

        genFuncletInfo.fiSpDelta1 = -(int)funcletFrameSizeAligned;
        genFuncletInfo.fiSpDelta2 = 0;

        assert(genFuncletInfo.fiSpDelta1 + genFuncletInfo.fiSpDelta2 == -(int)funcletFrameSizeAligned);
    }
    else
    {
        unsigned saveRegsPlusPSPAlignmentPad = saveRegsPlusPSPSizeAligned - saveRegsPlusPSPSize;
        assert((saveRegsPlusPSPAlignmentPad == 0) || (saveRegsPlusPSPAlignmentPad == REGSIZE_BYTES));

        if (genSaveFpLrWithAllCalleeSavedRegisters)
        {
            SP_to_FPLR_save_delta = funcletFrameSizeAligned - (2 /* FP, LR */ * REGSIZE_BYTES);
            if (compiler->info.compIsVarArgs)
            {
                SP_to_FPLR_save_delta -= MAX_REG_ARG * REGSIZE_BYTES;
            }

            SP_to_PSP_slot_delta =
                compiler->lvaOutgoingArgSpaceSize + funcletFrameAlignmentPad + saveRegsPlusPSPAlignmentPad;
            CallerSP_to_PSP_slot_delta = -(int)saveRegsPlusPSPSize;

            genFuncletInfo.fiFrameType = 5;
        }
        else
        {
            SP_to_FPLR_save_delta = outgoingArgSpaceAligned;
            SP_to_PSP_slot_delta = SP_to_FPLR_save_delta + 2 /* FP, LR */ * REGSIZE_BYTES + saveRegsPlusPSPAlignmentPad;
            CallerSP_to_PSP_slot_delta =
                -(int)(saveRegsPlusPSPSizeAligned - 2 /* FP, LR */ * REGSIZE_BYTES - saveRegsPlusPSPAlignmentPad);

            genFuncletInfo.fiFrameType = 3;
        }

        genFuncletInfo.fiSpDelta1 = -(int)saveRegsPlusPSPSizeAligned;
        genFuncletInfo.fiSpDelta2 = -(int)outgoingArgSpaceAligned;

        assert(genFuncletInfo.fiSpDelta1 + genFuncletInfo.fiSpDelta2 == -(int)maxFuncletFrameSizeAligned);
    }

    /* Now save it for future use */

    genFuncletInfo.fiSaveRegs                   = rsMaskSaveRegs;
    genFuncletInfo.fiSP_to_FPLR_save_delta      = SP_to_FPLR_save_delta;
    genFuncletInfo.fiSP_to_PSP_slot_delta       = SP_to_PSP_slot_delta;
    genFuncletInfo.fiSP_to_CalleeSave_delta     = SP_to_PSP_slot_delta + REGSIZE_BYTES;
    genFuncletInfo.fiCallerSP_to_PSP_slot_delta = CallerSP_to_PSP_slot_delta;

#ifdef DEBUG
    if (verbose)
    {
        printf("\n");
        printf("Funclet prolog / epilog info\n");
        printf("                        Save regs: ");
        dspRegMask(genFuncletInfo.fiSaveRegs);
        printf("\n");
        printf("    Function CallerSP-to-FP delta: %d\n", genFuncletInfo.fiFunction_CallerSP_to_FP_delta);
        printf("  SP to FP/LR save location delta: %d\n", genFuncletInfo.fiSP_to_FPLR_save_delta);
        printf("             SP to PSP slot delta: %d\n", genFuncletInfo.fiSP_to_PSP_slot_delta);
        printf("    SP to callee-saved area delta: %d\n", genFuncletInfo.fiSP_to_CalleeSave_delta);
        printf("      Caller SP to PSP slot delta: %d\n", genFuncletInfo.fiCallerSP_to_PSP_slot_delta);
        printf("                       Frame type: %d\n", genFuncletInfo.fiFrameType);
        printf("                       SP delta 1: %d\n", genFuncletInfo.fiSpDelta1);
        printf("                       SP delta 2: %d\n", genFuncletInfo.fiSpDelta2);

        if (compiler->lvaPSPSym != BAD_VAR_NUM)
        {
            if (CallerSP_to_PSP_slot_delta !=
                compiler->lvaGetCallerSPRelativeOffset(compiler->lvaPSPSym)) // for debugging
            {
                printf("lvaGetCallerSPRelativeOffset(lvaPSPSym): %d\n",
                       compiler->lvaGetCallerSPRelativeOffset(compiler->lvaPSPSym));
            }
        }
    }

    assert(genFuncletInfo.fiSP_to_FPLR_save_delta >= 0);
    assert(genFuncletInfo.fiSP_to_PSP_slot_delta >= 0);
    assert(genFuncletInfo.fiSP_to_CalleeSave_delta >= 0);
    assert(genFuncletInfo.fiCallerSP_to_PSP_slot_delta <= 0);

    if (compiler->lvaPSPSym != BAD_VAR_NUM)
    {
        assert(genFuncletInfo.fiCallerSP_to_PSP_slot_delta ==
               compiler->lvaGetCallerSPRelativeOffset(compiler->lvaPSPSym)); // same offset used in main function and
                                                                             // funclet!
    }
#endif // DEBUG
}

/*
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                           End Prolog / Epilog                             XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

BasicBlock* CodeGen::genCallFinally(BasicBlock* block)
{
    // Generate a call to the finally, like this:
    //      mov         x0,qword ptr [fp + 10H] / sp    // Load x0 with PSPSym, or sp if PSPSym is not used
    //      bl          finally-funclet
    //      b           finally-return                  // Only for non-retless finally calls
    // The 'b' can be a NOP if we're going to the next block.

    if (compiler->lvaPSPSym != BAD_VAR_NUM)
    {
        getEmitter()->emitIns_R_S(ins_Load(TYP_I_IMPL), EA_PTRSIZE, REG_R0, compiler->lvaPSPSym, 0);
    }
    else
    {
        getEmitter()->emitIns_R_R(INS_mov, EA_PTRSIZE, REG_R0, REG_SPBASE);
    }
    getEmitter()->emitIns_J(INS_bl_local, block->bbJumpDest);

    if (block->bbFlags & BBF_RETLESS_CALL)
    {
        // We have a retless call, and the last instruction generated was a call.
        // If the next block is in a different EH region (or is the end of the code
        // block), then we need to generate a breakpoint here (since it will never
        // get executed) to get proper unwind behavior.

        if ((block->bbNext == nullptr) || !BasicBlock::sameEHRegion(block, block->bbNext))
        {
            instGen(INS_bkpt); // This should never get executed
        }
    }
    else
    {
        // Because of the way the flowgraph is connected, the liveness info for this one instruction
        // after the call is not (can not be) correct in cases where a variable has a last use in the
        // handler.  So turn off GC reporting for this single instruction.
        getEmitter()->emitDisableGC();

        // Now go to where the finally funclet needs to return to.
        if (block->bbNext->bbJumpDest == block->bbNext->bbNext)
        {
            // Fall-through.
            // TODO-ARM64-CQ: Can we get rid of this instruction, and just have the call return directly
            // to the next instruction? This would depend on stack walking from within the finally
            // handler working without this instruction being in this special EH region.
            instGen(INS_nop);
        }
        else
        {
            inst_JMP(EJ_jmp, block->bbNext->bbJumpDest);
        }

        getEmitter()->emitEnableGC();
    }

    // The BBJ_ALWAYS is used because the BBJ_CALLFINALLY can't point to the
    // jump target using bbJumpDest - that is already used to point
    // to the finally block. So just skip past the BBJ_ALWAYS unless the
    // block is RETLESS.
    if (!(block->bbFlags & BBF_RETLESS_CALL))
    {
        assert(block->isBBCallAlwaysPair());
        block = block->bbNext;
    }
    return block;
}

void CodeGen::genEHCatchRet(BasicBlock* block)
{
    // For long address (default): `adrp + add` will be emitted.
    // For short address (proven later): `adr` will be emitted.
    getEmitter()->emitIns_R_L(INS_adr, EA_PTRSIZE, block->bbJumpDest, REG_INTRET);
}

//  move an immediate value into an integer register

void CodeGen::instGen_Set_Reg_To_Imm(emitAttr size, regNumber reg, ssize_t imm, insFlags flags)
{
    // reg cannot be a FP register
    assert(!genIsValidFloatReg(reg));
    if (!compiler->opts.compReloc)
    {
        size = EA_SIZE(size); // Strip any Reloc flags from size if we aren't doing relocs
    }

    if (EA_IS_RELOC(size))
    {
        // This emits a pair of adrp/add (two instructions) with fix-ups.
        getEmitter()->emitIns_R_AI(INS_adrp, size, reg, imm);
    }
    else if (imm == 0)
    {
        instGen_Set_Reg_To_Zero(size, reg, flags);
    }
    else
    {
        if (emitter::emitIns_valid_imm_for_mov(imm, size))
        {
            getEmitter()->emitIns_R_I(INS_mov, size, reg, imm);
        }
        else
        {
            // Arm64 allows any arbitrary 16-bit constant to be loaded into a register halfword
            // There are three forms
            //    movk which loads into any halfword preserving the remaining halfwords
            //    movz which loads into any halfword zeroing the remaining halfwords
            //    movn which loads into any halfword zeroing the remaining halfwords then bitwise inverting the register
            // In some cases it is preferable to use movn, because it has the side effect of filling the other halfwords
            // with ones

            // Determine whether movn or movz will require the fewest instructions to populate the immediate
            int preferMovn = 0;

            for (int i = (size == EA_8BYTE) ? 48 : 16; i >= 0; i -= 16)
            {
                if (uint16_t(imm >> i) == 0xffff)
                    ++preferMovn; // a single movk 0xffff could be skipped if movn was used
                else if (uint16_t(imm >> i) == 0x0000)
                    --preferMovn; // a single movk 0 could be skipped if movz was used
            }

            // Select the first instruction.  Any additional instruction will use movk
            instruction ins = (preferMovn > 0) ? INS_movn : INS_movz;

            // Initial movz or movn will fill the remaining bytes with the skipVal
            // This can allow skipping filling a halfword
            uint16_t skipVal = (preferMovn > 0) ? 0xffff : 0;

            unsigned bits = (size == EA_8BYTE) ? 64 : 32;

            // Iterate over imm examining 16 bits at a time
            for (unsigned i = 0; i < bits; i += 16)
            {
                uint16_t imm16 = uint16_t(imm >> i);

                if (imm16 != skipVal)
                {
                    if (ins == INS_movn)
                    {
                        // For the movn case, we need to bitwise invert the immediate.  This is because
                        //   (movn x0, ~imm16) === (movz x0, imm16; or x0, x0, #0xffff`ffff`ffff`0000)
                        imm16 = ~imm16;
                    }

                    getEmitter()->emitIns_R_I_I(ins, size, reg, imm16, i, INS_OPTS_LSL);

                    // Once the initial movz/movn is emitted the remaining instructions will all use movk
                    ins = INS_movk;
                }
            }

            // We must emit a movn or movz or we have not done anything
            // The cases which hit this assert should be (emitIns_valid_imm_for_mov() == true) and
            // should not be in this else condition
            assert(ins == INS_movk);
        }
        // The caller may have requested that the flags be set on this mov (rarely/never)
        if (flags == INS_FLAGS_SET)
        {
            getEmitter()->emitIns_R_I(INS_tst, size, reg, 0);
        }
    }

    regSet.verifyRegUsed(reg);
}

/***********************************************************************************
 *
 * Generate code to set a register 'targetReg' of type 'targetType' to the constant
 * specified by the constant (GT_CNS_INT or GT_CNS_DBL) in 'tree'. This does not call
 * genProduceReg() on the target register.
 */
void CodeGen::genSetRegToConst(regNumber targetReg, var_types targetType, GenTree* tree)
{
    switch (tree->gtOper)
    {
        case GT_CNS_INT:
        {
            // relocatable values tend to come down as a CNS_INT of native int type
            // so the line between these two opcodes is kind of blurry
            GenTreeIntConCommon* con    = tree->AsIntConCommon();
            ssize_t              cnsVal = con->IconValue();

            if (con->ImmedValNeedsReloc(compiler))
            {
                instGen_Set_Reg_To_Imm(EA_HANDLE_CNS_RELOC, targetReg, cnsVal);
                regSet.verifyRegUsed(targetReg);
            }
            else
            {
                genSetRegToIcon(targetReg, cnsVal, targetType);
            }
        }
        break;

        case GT_CNS_DBL:
        {
            emitter* emit       = getEmitter();
            emitAttr size       = emitActualTypeSize(tree);
            double   constValue = tree->AsDblCon()->gtDconVal;

            // Make sure we use "movi reg, 0x00"  only for positive zero (0.0) and not for negative zero (-0.0)
            if (*(__int64*)&constValue == 0)
            {
                // A faster/smaller way to generate 0.0
                // We will just zero out the entire vector register for both float and double
                emit->emitIns_R_I(INS_movi, EA_16BYTE, targetReg, 0x00, INS_OPTS_16B);
            }
            else if (emitter::emitIns_valid_imm_for_fmov(constValue))
            {
                // We can load the FP constant using the fmov FP-immediate for this constValue
                emit->emitIns_R_F(INS_fmov, size, targetReg, constValue);
            }
            else
            {
                // Get a temp integer register to compute long address.
                regNumber addrReg = tree->GetSingleTempReg();

                // We must load the FP constant from the constant pool
                // Emit a data section constant for the float or double constant.
                CORINFO_FIELD_HANDLE hnd = emit->emitFltOrDblConst(constValue, size);
                // For long address (default): `adrp + ldr + fmov` will be emitted.
                // For short address (proven later), `ldr` will be emitted.
                emit->emitIns_R_C(INS_ldr, size, targetReg, addrReg, hnd, 0);
            }
        }
        break;

        default:
            unreached();
    }
}

// Generate code to get the high N bits of a N*N=2N bit multiplication result
void CodeGen::genCodeForMulHi(GenTreeOp* treeNode)
{
    assert(!treeNode->gtOverflowEx());

    genConsumeOperands(treeNode);

    regNumber targetReg  = treeNode->gtRegNum;
    var_types targetType = treeNode->TypeGet();
    emitter*  emit       = getEmitter();
    emitAttr  attr       = emitActualTypeSize(treeNode);
    unsigned  isUnsigned = (treeNode->gtFlags & GTF_UNSIGNED);

    GenTree* op1 = treeNode->gtGetOp1();
    GenTree* op2 = treeNode->gtGetOp2();

    assert(!varTypeIsFloating(targetType));

    // The arithmetic node must be sitting in a register (since it's not contained)
    assert(targetReg != REG_NA);

    if (EA_SIZE(attr) == EA_8BYTE)
    {
        instruction ins = isUnsigned ? INS_umulh : INS_smulh;

        regNumber r = emit->emitInsTernary(ins, attr, treeNode, op1, op2);

        assert(r == targetReg);
    }
    else
    {
        assert(EA_SIZE(attr) == EA_4BYTE);

        instruction ins = isUnsigned ? INS_umull : INS_smull;

        regNumber r = emit->emitInsTernary(ins, EA_4BYTE, treeNode, op1, op2);

        emit->emitIns_R_R_I(isUnsigned ? INS_lsr : INS_asr, EA_8BYTE, targetReg, targetReg, 32);
    }

    genProduceReg(treeNode);
}

// Generate code for ADD, SUB, MUL, DIV, UDIV, AND, OR and XOR
// This method is expected to have called genConsumeOperands() before calling it.
void CodeGen::genCodeForBinary(GenTreeOp* treeNode)
{
    const genTreeOps oper       = treeNode->OperGet();
    regNumber        targetReg  = treeNode->gtRegNum;
    var_types        targetType = treeNode->TypeGet();
    emitter*         emit       = getEmitter();

    assert(oper == GT_ADD || oper == GT_SUB || oper == GT_MUL || oper == GT_DIV || oper == GT_UDIV || oper == GT_AND ||
           oper == GT_OR || oper == GT_XOR);

    GenTree*    op1 = treeNode->gtGetOp1();
    GenTree*    op2 = treeNode->gtGetOp2();
    instruction ins = genGetInsForOper(treeNode->OperGet(), targetType);

    if ((treeNode->gtFlags & GTF_SET_FLAGS) != 0)
    {
        switch (oper)
        {
            case GT_ADD:
                ins = INS_adds;
                break;
            case GT_SUB:
                ins = INS_subs;
                break;
            case GT_AND:
                ins = INS_ands;
                break;
            default:
                noway_assert(!"Unexpected BinaryOp with GTF_SET_FLAGS set");
        }
    }

    // The arithmetic node must be sitting in a register (since it's not contained)
    assert(targetReg != REG_NA);

    regNumber r = emit->emitInsTernary(ins, emitActualTypeSize(treeNode), treeNode, op1, op2);
    assert(r == targetReg);

    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genCodeForLclVar: Produce code for a GT_LCL_VAR node.
//
// Arguments:
//    tree - the GT_LCL_VAR node
//
void CodeGen::genCodeForLclVar(GenTreeLclVar* tree)
{
    var_types targetType = tree->TypeGet();
    emitter*  emit       = getEmitter();

    unsigned varNum = tree->gtLclNum;
    assert(varNum < compiler->lvaCount);
    LclVarDsc* varDsc         = &(compiler->lvaTable[varNum]);
    bool       isRegCandidate = varDsc->lvIsRegCandidate();

    // lcl_vars are not defs
    assert((tree->gtFlags & GTF_VAR_DEF) == 0);

    // If this is a register candidate that has been spilled, genConsumeReg() will
    // reload it at the point of use.  Otherwise, if it's not in a register, we load it here.

    if (!isRegCandidate && !(tree->gtFlags & GTF_SPILLED))
    {
        // targetType must be a normal scalar type and not a TYP_STRUCT
        assert(targetType != TYP_STRUCT);

        instruction ins  = ins_Load(targetType);
        emitAttr    attr = emitTypeSize(targetType);

        attr = varTypeIsFloating(targetType) ? attr : emit->emitInsAdjustLoadStoreAttr(ins, attr);

        emit->emitIns_R_S(ins, attr, tree->gtRegNum, varNum, 0);
        genProduceReg(tree);
    }
}

//------------------------------------------------------------------------
// genCodeForStoreLclFld: Produce code for a GT_STORE_LCL_FLD node.
//
// Arguments:
//    tree - the GT_STORE_LCL_FLD node
//
void CodeGen::genCodeForStoreLclFld(GenTreeLclFld* tree)
{
    var_types targetType = tree->TypeGet();
    regNumber targetReg  = tree->gtRegNum;
    emitter*  emit       = getEmitter();
    noway_assert(targetType != TYP_STRUCT);

#ifdef FEATURE_SIMD
    // storing of TYP_SIMD12 (i.e. Vector3) field
    if (tree->TypeGet() == TYP_SIMD12)
    {
        genStoreLclTypeSIMD12(tree);
        return;
    }
#endif // FEATURE_SIMD

    // record the offset
    unsigned offset = tree->gtLclOffs;

    // We must have a stack store with GT_STORE_LCL_FLD
    noway_assert(targetReg == REG_NA);

    unsigned varNum = tree->gtLclNum;
    assert(varNum < compiler->lvaCount);
    LclVarDsc* varDsc = &(compiler->lvaTable[varNum]);

    // Ensure that lclVar nodes are typed correctly.
    assert(!varDsc->lvNormalizeOnStore() || targetType == genActualType(varDsc->TypeGet()));

    GenTree* data = tree->gtOp1;
    genConsumeRegs(data);

    regNumber dataReg = REG_NA;
    if (data->isContainedIntOrIImmed())
    {
        assert(data->IsIntegralConst(0));
        dataReg = REG_ZR;
    }
    else
    {
        assert(!data->isContained());
        dataReg = data->gtRegNum;
    }
    assert(dataReg != REG_NA);

    instruction ins = ins_Store(targetType);

    emitAttr attr = emitTypeSize(targetType);

    attr = varTypeIsFloating(targetType) ? attr : emit->emitInsAdjustLoadStoreAttr(ins, attr);

    emit->emitIns_S_R(ins, attr, dataReg, varNum, offset);

    genUpdateLife(tree);

    varDsc->lvRegNum = REG_STK;
}

//------------------------------------------------------------------------
// genCodeForStoreLclVar: Produce code for a GT_STORE_LCL_VAR node.
//
// Arguments:
//    tree - the GT_STORE_LCL_VAR node
//
void CodeGen::genCodeForStoreLclVar(GenTreeLclVar* tree)
{
    var_types targetType = tree->TypeGet();
    regNumber targetReg  = tree->gtRegNum;
    emitter*  emit       = getEmitter();

    unsigned varNum = tree->gtLclNum;
    assert(varNum < compiler->lvaCount);
    LclVarDsc* varDsc = &(compiler->lvaTable[varNum]);

    // Ensure that lclVar nodes are typed correctly.
    assert(!varDsc->lvNormalizeOnStore() || targetType == genActualType(varDsc->TypeGet()));

    GenTree* data = tree->gtOp1;

    // var = call, where call returns a multi-reg return value
    // case is handled separately.
    if (data->gtSkipReloadOrCopy()->IsMultiRegCall())
    {
        genMultiRegCallStoreToLocal(tree);
    }
    else
    {
#ifdef FEATURE_SIMD
        // storing of TYP_SIMD12 (i.e. Vector3) field
        if (tree->TypeGet() == TYP_SIMD12)
        {
            genStoreLclTypeSIMD12(tree);
            return;
        }
#endif // FEATURE_SIMD

        genConsumeRegs(data);

        regNumber dataReg = REG_NA;
        if (data->isContainedIntOrIImmed())
        {
            // This is only possible for a zero-init.
            assert(data->IsIntegralConst(0));

            if (varTypeIsSIMD(targetType))
            {
                assert(targetReg != REG_NA);
                getEmitter()->emitIns_R_I(INS_movi, EA_16BYTE, targetReg, 0x00, INS_OPTS_16B);
                genProduceReg(tree);
                return;
            }

            dataReg = REG_ZR;
        }
        else
        {
            assert(!data->isContained());
            dataReg = data->gtRegNum;
        }
        assert(dataReg != REG_NA);

        if (targetReg == REG_NA) // store into stack based LclVar
        {
            inst_set_SV_var(tree);

            instruction ins  = ins_Store(targetType);
            emitAttr    attr = emitTypeSize(targetType);

            attr = varTypeIsFloating(targetType) ? attr : emit->emitInsAdjustLoadStoreAttr(ins, attr);

            emit->emitIns_S_R(ins, attr, dataReg, varNum, /* offset */ 0);

            genUpdateLife(tree);

            varDsc->lvRegNum = REG_STK;
        }
        else // store into register (i.e move into register)
        {
            if (dataReg != targetReg)
            {
                // Assign into targetReg when dataReg (from op1) is not the same register
                inst_RV_RV(ins_Copy(targetType), targetReg, dataReg, targetType);
            }
            genProduceReg(tree);
        }
    }
}

//------------------------------------------------------------------------
// genSimpleReturn: Generates code for simple return statement for arm64.
//
// Note: treeNode's and op1's registers are already consumed.
//
// Arguments:
//    treeNode - The GT_RETURN or GT_RETFILT tree node with non-struct and non-void type
//
// Return Value:
//    None
//
void CodeGen::genSimpleReturn(GenTree* treeNode)
{
    assert(treeNode->OperGet() == GT_RETURN || treeNode->OperGet() == GT_RETFILT);
    GenTree*  op1        = treeNode->gtGetOp1();
    var_types targetType = treeNode->TypeGet();

    assert(!isStructReturn(treeNode));
    assert(targetType != TYP_VOID);

    regNumber retReg = varTypeIsFloating(treeNode) ? REG_FLOATRET : REG_INTRET;

    bool movRequired = (op1->gtRegNum != retReg);

    if (!movRequired)
    {
        if (op1->OperGet() == GT_LCL_VAR)
        {
            GenTreeLclVarCommon* lcl            = op1->AsLclVarCommon();
            bool                 isRegCandidate = compiler->lvaTable[lcl->gtLclNum].lvIsRegCandidate();
            if (isRegCandidate && ((op1->gtFlags & GTF_SPILLED) == 0))
            {
                // We may need to generate a zero-extending mov instruction to load the value from this GT_LCL_VAR

                unsigned   lclNum  = lcl->gtLclNum;
                LclVarDsc* varDsc  = &(compiler->lvaTable[lclNum]);
                var_types  op1Type = genActualType(op1->TypeGet());
                var_types  lclType = genActualType(varDsc->TypeGet());

                if (genTypeSize(op1Type) < genTypeSize(lclType))
                {
                    movRequired = true;
                }
            }
        }
    }
    if (movRequired)
    {
        emitAttr attr = emitActualTypeSize(targetType);
        getEmitter()->emitIns_R_R(INS_mov, attr, retReg, op1->gtRegNum);
    }
}

/***********************************************************************************************
 *  Generate code for localloc
 */
void CodeGen::genLclHeap(GenTree* tree)
{
    assert(tree->OperGet() == GT_LCLHEAP);
    assert(compiler->compLocallocUsed);

    GenTree* size = tree->gtOp.gtOp1;
    noway_assert((genActualType(size->gtType) == TYP_INT) || (genActualType(size->gtType) == TYP_I_IMPL));

    regNumber   targetReg       = tree->gtRegNum;
    regNumber   regCnt          = REG_NA;
    regNumber   pspSymReg       = REG_NA;
    var_types   type            = genActualType(size->gtType);
    emitAttr    easz            = emitTypeSize(type);
    BasicBlock* endLabel        = nullptr;
    BasicBlock* loop            = nullptr;
    unsigned    stackAdjustment = 0;

    noway_assert(isFramePointerUsed()); // localloc requires Frame Pointer to be established since SP changes
    noway_assert(genStackLevel == 0);   // Can't have anything on the stack

    // compute the amount of memory to allocate to properly STACK_ALIGN.
    size_t amount = 0;
    if (size->IsCnsIntOrI())
    {
        // If size is a constant, then it must be contained.
        assert(size->isContained());

        // If amount is zero then return null in targetReg
        amount = size->gtIntCon.gtIconVal;
        if (amount == 0)
        {
            instGen_Set_Reg_To_Zero(EA_PTRSIZE, targetReg);
            goto BAILOUT;
        }

        // 'amount' is the total number of bytes to localloc to properly STACK_ALIGN
        amount = AlignUp(amount, STACK_ALIGN);
    }
    else
    {
        // If 0 bail out by returning null in targetReg
        genConsumeRegAndCopy(size, targetReg);
        endLabel = genCreateTempLabel();
        getEmitter()->emitIns_R_R(INS_tst, easz, targetReg, targetReg);
        inst_JMP(EJ_eq, endLabel);

        // Compute the size of the block to allocate and perform alignment.
        // If compInitMem=true, we can reuse targetReg as regcnt,
        // since we don't need any internal registers.
        if (compiler->info.compInitMem)
        {
            assert(tree->AvailableTempRegCount() == 0);
            regCnt = targetReg;
        }
        else
        {
            regCnt = tree->ExtractTempReg();
            if (regCnt != targetReg)
            {
                inst_RV_RV(INS_mov, regCnt, targetReg, size->TypeGet());
            }
        }

        // Align to STACK_ALIGN
        // regCnt will be the total number of bytes to localloc
        inst_RV_IV(INS_add, regCnt, (STACK_ALIGN - 1), emitActualTypeSize(type));
        inst_RV_IV(INS_and, regCnt, ~(STACK_ALIGN - 1), emitActualTypeSize(type));
    }

    stackAdjustment = 0;

    // If we have an outgoing arg area then we must adjust the SP by popping off the
    // outgoing arg area. We will restore it right before we return from this method.
    //
    // Localloc returns stack space that aligned to STACK_ALIGN bytes. The following
    // are the cases that need to be handled:
    //   i) Method has out-going arg area.
    //      It is guaranteed that size of out-going arg area is STACK_ALIGN'ed (see fgMorphArgs).
    //      Therefore, we will pop off the out-going arg area from the stack pointer before allocating the localloc
    //      space.
    //  ii) Method has no out-going arg area.
    //      Nothing to pop off from the stack.
    if (compiler->lvaOutgoingArgSpaceSize > 0)
    {
        assert((compiler->lvaOutgoingArgSpaceSize % STACK_ALIGN) == 0); // This must be true for the stack to remain
                                                                        // aligned
        genInstrWithConstant(INS_add, EA_PTRSIZE, REG_SPBASE, REG_SPBASE, compiler->lvaOutgoingArgSpaceSize,
                             rsGetRsvdReg());
        stackAdjustment += compiler->lvaOutgoingArgSpaceSize;
    }

    if (size->IsCnsIntOrI())
    {
        // We should reach here only for non-zero, constant size allocations.
        assert(amount > 0);

        // For small allocations we will generate up to four stp instructions, to zero 16 to 64 bytes.
        static_assert_no_msg(STACK_ALIGN == (REGSIZE_BYTES * 2));
        assert(amount % (REGSIZE_BYTES * 2) == 0); // stp stores two registers at a time
        size_t stpCount = amount / (REGSIZE_BYTES * 2);
        if (stpCount <= 4)
        {
            while (stpCount != 0)
            {
                // We can use pre-indexed addressing.
                // stp ZR, ZR, [SP, #-16]!   // STACK_ALIGN is 16
                getEmitter()->emitIns_R_R_R_I(INS_stp, EA_PTRSIZE, REG_ZR, REG_ZR, REG_SPBASE, -16, INS_OPTS_PRE_INDEX);
                stpCount -= 1;
            }

            goto ALLOC_DONE;
        }
        else if (!compiler->info.compInitMem && (amount < compiler->eeGetPageSize())) // must be < not <=
        {
            // Since the size is less than a page, simply adjust the SP value.
            // The SP might already be in the guard page, so we must touch it BEFORE
            // the alloc, not after.

            // ldr wz, [SP, #0]
            getEmitter()->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_ZR, REG_SP, 0);

            inst_RV_IV(INS_sub, REG_SP, amount, EA_PTRSIZE);

            goto ALLOC_DONE;
        }

        // else, "mov regCnt, amount"
        // If compInitMem=true, we can reuse targetReg as regcnt.
        // Since size is a constant, regCnt is not yet initialized.
        assert(regCnt == REG_NA);
        if (compiler->info.compInitMem)
        {
            assert(tree->AvailableTempRegCount() == 0);
            regCnt = targetReg;
        }
        else
        {
            regCnt = tree->ExtractTempReg();
        }
        genSetRegToIcon(regCnt, amount, ((unsigned int)amount == amount) ? TYP_INT : TYP_LONG);
    }

    if (compiler->info.compInitMem)
    {
        BasicBlock* loop = genCreateTempLabel();

        // At this point 'regCnt' is set to the total number of bytes to locAlloc.
        // Since we have to zero out the allocated memory AND ensure that the stack pointer is always valid
        // by tickling the pages, we will just push 0's on the stack.
        //
        // Note: regCnt is guaranteed to be even on Amd64 since STACK_ALIGN/TARGET_POINTER_SIZE = 2
        // and localloc size is a multiple of STACK_ALIGN.

        // Loop:
        genDefineTempLabel(loop);

        // We can use pre-indexed addressing.
        // stp ZR, ZR, [SP, #-16]!
        getEmitter()->emitIns_R_R_R_I(INS_stp, EA_PTRSIZE, REG_ZR, REG_ZR, REG_SPBASE, -16, INS_OPTS_PRE_INDEX);

        // If not done, loop
        // Note that regCnt is the number of bytes to stack allocate.
        // Therefore we need to subtract 16 from regcnt here.
        assert(genIsValidIntReg(regCnt));
        inst_RV_IV(INS_subs, regCnt, 16, emitActualTypeSize(type));
        inst_JMP(EJ_ne, loop);
    }
    else
    {
        // At this point 'regCnt' is set to the total number of bytes to localloc.
        //
        // We don't need to zero out the allocated memory. However, we do have
        // to tickle the pages to ensure that SP is always valid and is
        // in sync with the "stack guard page".  Note that in the worst
        // case SP is on the last byte of the guard page.  Thus you must
        // touch SP-0 first not SP-0x1000.
        //
        // This is similar to the prolog code in CodeGen::genAllocLclFrame().
        //
        // Note that we go through a few hoops so that SP never points to
        // illegal pages at any time during the tickling process.
        //
        //       subs  regCnt, SP, regCnt      // regCnt now holds ultimate SP
        //       bvc   Loop                    // result is smaller than original SP (no wrap around)
        //       mov   regCnt, #0              // Overflow, pick lowest possible value
        //
        //  Loop:
        //       ldr   wzr, [SP + 0]           // tickle the page - read from the page
        //       sub   regTmp, SP, PAGE_SIZE   // decrement SP by eeGetPageSize()
        //       cmp   regTmp, regCnt
        //       jb    Done
        //       mov   SP, regTmp
        //       j     Loop
        //
        //  Done:
        //       mov   SP, regCnt
        //

        // Setup the regTmp
        regNumber regTmp = tree->GetSingleTempReg();

        BasicBlock* loop = genCreateTempLabel();
        BasicBlock* done = genCreateTempLabel();

        //       subs  regCnt, SP, regCnt      // regCnt now holds ultimate SP
        getEmitter()->emitIns_R_R_R(INS_subs, EA_PTRSIZE, regCnt, REG_SPBASE, regCnt);

        inst_JMP(EJ_vc, loop); // branch if the V flag is not set

        // Overflow, set regCnt to lowest possible value
        instGen_Set_Reg_To_Zero(EA_PTRSIZE, regCnt);

        genDefineTempLabel(loop);

        // tickle the page - Read from the updated SP - this triggers a page fault when on the guard page
        getEmitter()->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_ZR, REG_SPBASE, 0);

        // decrement SP by eeGetPageSize()
        getEmitter()->emitIns_R_R_I(INS_sub, EA_PTRSIZE, regTmp, REG_SPBASE, compiler->eeGetPageSize());

        getEmitter()->emitIns_R_R(INS_cmp, EA_PTRSIZE, regTmp, regCnt);
        inst_JMP(EJ_lo, done);

        // Update SP to be at the next page of stack that we will tickle
        getEmitter()->emitIns_R_R(INS_mov, EA_PTRSIZE, REG_SPBASE, regTmp);

        // Jump to loop and tickle new stack address
        inst_JMP(EJ_jmp, loop);

        // Done with stack tickle loop
        genDefineTempLabel(done);

        // Now just move the final value to SP
        getEmitter()->emitIns_R_R(INS_mov, EA_PTRSIZE, REG_SPBASE, regCnt);
    }

ALLOC_DONE:
    // Re-adjust SP to allocate out-going arg area
    if (stackAdjustment != 0)
    {
        assert((stackAdjustment % STACK_ALIGN) == 0); // This must be true for the stack to remain aligned
        assert(stackAdjustment > 0);
        genInstrWithConstant(INS_sub, EA_PTRSIZE, REG_SPBASE, REG_SPBASE, (ssize_t)stackAdjustment, rsGetRsvdReg());

        // Return the stackalloc'ed address in result register.
        // TargetReg = SP + stackAdjustment.
        //
        genInstrWithConstant(INS_add, EA_PTRSIZE, targetReg, REG_SPBASE, (ssize_t)stackAdjustment, rsGetRsvdReg());
    }
    else // stackAdjustment == 0
    {
        // Move the final value of SP to targetReg
        inst_RV_RV(INS_mov, targetReg, REG_SPBASE);
    }

BAILOUT:
    if (endLabel != nullptr)
        genDefineTempLabel(endLabel);

    genProduceReg(tree);
}

//------------------------------------------------------------------------
// genCodeForNegNot: Produce code for a GT_NEG/GT_NOT node.
//
// Arguments:
//    tree - the node
//
void CodeGen::genCodeForNegNot(GenTree* tree)
{
    assert(tree->OperIs(GT_NEG, GT_NOT));

    var_types targetType = tree->TypeGet();

    assert(!tree->OperIs(GT_NOT) || !varTypeIsFloating(targetType));

    regNumber   targetReg = tree->gtRegNum;
    instruction ins       = genGetInsForOper(tree->OperGet(), targetType);

    // The arithmetic node must be sitting in a register (since it's not contained)
    assert(!tree->isContained());
    // The dst can only be a register.
    assert(targetReg != REG_NA);

    GenTree* operand = tree->gtGetOp1();
    assert(!operand->isContained());
    // The src must be a register.
    regNumber operandReg = genConsumeReg(operand);

    getEmitter()->emitIns_R_R(ins, emitActualTypeSize(tree), targetReg, operandReg);

    genProduceReg(tree);
}

//------------------------------------------------------------------------
// genCodeForDivMod: Produce code for a GT_DIV/GT_UDIV node. We don't see MOD:
// (1) integer MOD is morphed into a sequence of sub, mul, div in fgMorph;
// (2) float/double MOD is morphed into a helper call by front-end.
//
// Arguments:
//    tree - the node
//
void CodeGen::genCodeForDivMod(GenTreeOp* tree)
{
    assert(tree->OperIs(GT_DIV, GT_UDIV));

    var_types targetType = tree->TypeGet();
    emitter*  emit       = getEmitter();

    genConsumeOperands(tree);

    if (varTypeIsFloating(targetType))
    {
        // Floating point divide never raises an exception
        genCodeForBinary(tree);
    }
    else // an integer divide operation
    {
        GenTree* divisorOp = tree->gtGetOp2();
        emitAttr size      = EA_ATTR(genTypeSize(genActualType(tree->TypeGet())));

        if (divisorOp->IsIntegralConst(0))
        {
            // We unconditionally throw a divide by zero exception
            genJumpToThrowHlpBlk(EJ_jmp, SCK_DIV_BY_ZERO);

            // We still need to call genProduceReg
            genProduceReg(tree);
        }
        else // the divisor is not the constant zero
        {
            regNumber divisorReg = divisorOp->gtRegNum;

            // Generate the require runtime checks for GT_DIV or GT_UDIV
            if (tree->gtOper == GT_DIV)
            {
                BasicBlock* sdivLabel = genCreateTempLabel();

                // Two possible exceptions:
                //     (AnyVal /  0) => DivideByZeroException
                //     (MinInt / -1) => ArithmeticException
                //
                bool checkDividend = true;

                // Do we have an immediate for the 'divisorOp'?
                //
                if (divisorOp->IsCnsIntOrI())
                {
                    GenTreeIntConCommon* intConstTree  = divisorOp->AsIntConCommon();
                    ssize_t              intConstValue = intConstTree->IconValue();
                    assert(intConstValue != 0); // already checked above by IsIntegralConst(0)
                    if (intConstValue != -1)
                    {
                        checkDividend = false; // We statically know that the dividend is not -1
                    }
                }
                else // insert check for divison by zero
                {
                    // Check if the divisor is zero throw a DivideByZeroException
                    emit->emitIns_R_I(INS_cmp, size, divisorReg, 0);
                    genJumpToThrowHlpBlk(EJ_eq, SCK_DIV_BY_ZERO);
                }

                if (checkDividend)
                {
                    // Check if the divisor is not -1 branch to 'sdivLabel'
                    emit->emitIns_R_I(INS_cmp, size, divisorReg, -1);

                    inst_JMP(EJ_ne, sdivLabel);
                    // If control flow continues past here the 'divisorReg' is known to be -1

                    regNumber dividendReg = tree->gtGetOp1()->gtRegNum;
                    // At this point the divisor is known to be -1
                    //
                    // Issue the 'adds  zr, dividendReg, dividendReg' instruction
                    // this will set both the Z and V flags only when dividendReg is MinInt
                    //
                    emit->emitIns_R_R_R(INS_adds, size, REG_ZR, dividendReg, dividendReg);
                    inst_JMP(EJ_ne, sdivLabel);                   // goto sdiv if the Z flag is clear
                    genJumpToThrowHlpBlk(EJ_vs, SCK_ARITH_EXCPN); // if the V flags is set throw
                                                                  // ArithmeticException

                    genDefineTempLabel(sdivLabel);
                }
                genCodeForBinary(tree); // Generate the sdiv instruction
            }
            else // (tree->gtOper == GT_UDIV)
            {
                // Only one possible exception
                //     (AnyVal /  0) => DivideByZeroException
                //
                // Note that division by the constant 0 was already checked for above by the
                // op2->IsIntegralConst(0) check
                //
                if (!divisorOp->IsCnsIntOrI())
                {
                    // divisorOp is not a constant, so it could be zero
                    //
                    emit->emitIns_R_I(INS_cmp, size, divisorReg, 0);
                    genJumpToThrowHlpBlk(EJ_eq, SCK_DIV_BY_ZERO);
                }
                genCodeForBinary(tree);
            }
        }
    }
}

// Generate code for InitBlk by performing a loop unroll
// Preconditions:
//   a) Both the size and fill byte value are integer constants.
//   b) The size of the struct to initialize is smaller than INITBLK_UNROLL_LIMIT bytes.
void CodeGen::genCodeForInitBlkUnroll(GenTreeBlk* initBlkNode)
{
    // Make sure we got the arguments of the initblk/initobj operation in the right registers
    unsigned size    = initBlkNode->Size();
    GenTree* dstAddr = initBlkNode->Addr();
    GenTree* initVal = initBlkNode->Data();
    if (initVal->OperIsInitVal())
    {
        initVal = initVal->gtGetOp1();
    }

    assert(dstAddr->isUsedFromReg());
    assert((initVal->isUsedFromReg() && !initVal->IsIntegralConst(0)) || initVal->IsIntegralConst(0));
    assert(size != 0);
    assert(size <= INITBLK_UNROLL_LIMIT);

    emitter* emit = getEmitter();

    genConsumeOperands(initBlkNode);

    if (initBlkNode->gtFlags & GTF_BLK_VOLATILE)
    {
        // issue a full memory barrier before a volatile initBlockUnroll operation
        instGen_MemoryBarrier();
    }

    regNumber valReg = initVal->IsIntegralConst(0) ? REG_ZR : initVal->gtRegNum;

    assert(!initVal->IsIntegralConst(0) || (valReg == REG_ZR));

    unsigned offset = 0;

    // Perform an unroll using stp.
    if (size >= 2 * REGSIZE_BYTES)
    {
        // Determine how many 16 byte slots
        size_t slots = size / (2 * REGSIZE_BYTES);

        while (slots-- > 0)
        {
            emit->emitIns_R_R_R_I(INS_stp, EA_8BYTE, valReg, valReg, dstAddr->gtRegNum, offset);
            offset += (2 * REGSIZE_BYTES);
        }
    }

    // Fill the remainder (15 bytes or less) if there's any.
    if ((size & 0xf) != 0)
    {
        if ((size & 8) != 0)
        {
            emit->emitIns_R_R_I(INS_str, EA_8BYTE, valReg, dstAddr->gtRegNum, offset);
            offset += 8;
        }
        if ((size & 4) != 0)
        {
            emit->emitIns_R_R_I(INS_str, EA_4BYTE, valReg, dstAddr->gtRegNum, offset);
            offset += 4;
        }
        if ((size & 2) != 0)
        {
            emit->emitIns_R_R_I(INS_strh, EA_2BYTE, valReg, dstAddr->gtRegNum, offset);
            offset += 2;
        }
        if ((size & 1) != 0)
        {
            emit->emitIns_R_R_I(INS_strb, EA_1BYTE, valReg, dstAddr->gtRegNum, offset);
        }
    }
}

// Generate code for a load pair from some address + offset
//   base: tree node which can be either a local address or arbitrary node
//   offset: distance from the base from which to load
void CodeGen::genCodeForLoadPairOffset(regNumber dst, regNumber dst2, GenTree* base, unsigned offset)
{
    emitter* emit = getEmitter();

    if (base->OperIsLocalAddr())
    {
        if (base->gtOper == GT_LCL_FLD_ADDR)
            offset += base->gtLclFld.gtLclOffs;

        emit->emitIns_R_R_S_S(INS_ldp, EA_8BYTE, EA_8BYTE, dst, dst2, base->gtLclVarCommon.gtLclNum, offset);
    }
    else
    {
        emit->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, dst, dst2, base->gtRegNum, offset);
    }
}

// Generate code for a store pair to some address + offset
//   base: tree node which can be either a local address or arbitrary node
//   offset: distance from the base from which to load
void CodeGen::genCodeForStorePairOffset(regNumber src, regNumber src2, GenTree* base, unsigned offset)
{
    emitter* emit = getEmitter();

    if (base->OperIsLocalAddr())
    {
        if (base->gtOper == GT_LCL_FLD_ADDR)
            offset += base->gtLclFld.gtLclOffs;

        emit->emitIns_S_S_R_R(INS_stp, EA_8BYTE, EA_8BYTE, src, src2, base->gtLclVarCommon.gtLclNum, offset);
    }
    else
    {
        emit->emitIns_R_R_R_I(INS_stp, EA_8BYTE, src, src2, base->gtRegNum, offset);
    }
}

// Generate code for CpObj nodes wich copy structs that have interleaved
// GC pointers.
// For this case we'll generate a sequence of loads/stores in the case of struct
// slots that don't contain GC pointers.  The generated code will look like:
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
//
// In the case of a GC-Pointer we'll call the ByRef write barrier helper
// who happens to use the same registers as the previous call to maintain
// the same register requirements and register killsets:
// bl CORINFO_HELP_ASSIGN_BYREF
//
// So finally an example would look like this:
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
// bl CORINFO_HELP_ASSIGN_BYREF
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
// bl CORINFO_HELP_ASSIGN_BYREF
// ldr tempReg, [R13, #8]
// str tempReg, [R14, #8]
void CodeGen::genCodeForCpObj(GenTreeObj* cpObjNode)
{
    GenTree*  dstAddr       = cpObjNode->Addr();
    GenTree*  source        = cpObjNode->Data();
    var_types srcAddrType   = TYP_BYREF;
    bool      sourceIsLocal = false;

    assert(source->isContained());
    if (source->gtOper == GT_IND)
    {
        GenTree* srcAddr = source->gtGetOp1();
        assert(!srcAddr->isContained());
        srcAddrType = srcAddr->TypeGet();
    }
    else
    {
        noway_assert(source->IsLocal());
        sourceIsLocal = true;
    }

    bool dstOnStack = dstAddr->gtSkipReloadOrCopy()->OperIsLocalAddr();

#ifdef DEBUG
    assert(!dstAddr->isContained());

    // This GenTree node has data about GC pointers, this means we're dealing
    // with CpObj.
    assert(cpObjNode->gtGcPtrCount > 0);
#endif // DEBUG

    // Consume the operands and get them into the right registers.
    // They may now contain gc pointers (depending on their type; gcMarkRegPtrVal will "do the right thing").
    genConsumeBlockOp(cpObjNode, REG_WRITE_BARRIER_DST_BYREF, REG_WRITE_BARRIER_SRC_BYREF, REG_NA);
    gcInfo.gcMarkRegPtrVal(REG_WRITE_BARRIER_SRC_BYREF, srcAddrType);
    gcInfo.gcMarkRegPtrVal(REG_WRITE_BARRIER_DST_BYREF, dstAddr->TypeGet());

    unsigned slots = cpObjNode->gtSlots;

    // Temp register(s) used to perform the sequence of loads and stores.
    regNumber tmpReg  = cpObjNode->ExtractTempReg();
    regNumber tmpReg2 = REG_NA;

    assert(genIsValidIntReg(tmpReg));
    assert(tmpReg != REG_WRITE_BARRIER_SRC_BYREF);
    assert(tmpReg != REG_WRITE_BARRIER_DST_BYREF);

    if (slots > 1)
    {
        tmpReg2 = cpObjNode->GetSingleTempReg();
        assert(tmpReg2 != tmpReg);
        assert(genIsValidIntReg(tmpReg2));
        assert(tmpReg2 != REG_WRITE_BARRIER_DST_BYREF);
        assert(tmpReg2 != REG_WRITE_BARRIER_SRC_BYREF);
    }

    if (cpObjNode->gtFlags & GTF_BLK_VOLATILE)
    {
        // issue a full memory barrier before a volatile CpObj operation
        instGen_MemoryBarrier();
    }

    emitter* emit = getEmitter();

    BYTE* gcPtrs = cpObjNode->gtGcPtrs;

    // If we can prove it's on the stack we don't need to use the write barrier.
    if (dstOnStack)
    {
        unsigned i = 0;
        // Check if two or more remaining slots and use a ldp/stp sequence
        while (i < slots - 1)
        {
            emitAttr attr0 = emitTypeSize(compiler->getJitGCType(gcPtrs[i + 0]));
            emitAttr attr1 = emitTypeSize(compiler->getJitGCType(gcPtrs[i + 1]));

            emit->emitIns_R_R_R_I(INS_ldp, attr0, tmpReg, tmpReg2, REG_WRITE_BARRIER_SRC_BYREF, 2 * TARGET_POINTER_SIZE,
                                  INS_OPTS_POST_INDEX, attr1);
            emit->emitIns_R_R_R_I(INS_stp, attr0, tmpReg, tmpReg2, REG_WRITE_BARRIER_DST_BYREF, 2 * TARGET_POINTER_SIZE,
                                  INS_OPTS_POST_INDEX, attr1);
            i += 2;
        }

        // Use a ldr/str sequence for the last remainder
        if (i < slots)
        {
            emitAttr attr0 = emitTypeSize(compiler->getJitGCType(gcPtrs[i + 0]));

            emit->emitIns_R_R_I(INS_ldr, attr0, tmpReg, REG_WRITE_BARRIER_SRC_BYREF, TARGET_POINTER_SIZE,
                                INS_OPTS_POST_INDEX);
            emit->emitIns_R_R_I(INS_str, attr0, tmpReg, REG_WRITE_BARRIER_DST_BYREF, TARGET_POINTER_SIZE,
                                INS_OPTS_POST_INDEX);
        }
    }
    else
    {
        unsigned gcPtrCount = cpObjNode->gtGcPtrCount;

        unsigned i = 0;
        while (i < slots)
        {
            switch (gcPtrs[i])
            {
                case TYPE_GC_NONE:
                    // Check if the next slot's type is also TYP_GC_NONE and use ldp/stp
                    if ((i + 1 < slots) && (gcPtrs[i + 1] == TYPE_GC_NONE))
                    {
                        emit->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, tmpReg, tmpReg2, REG_WRITE_BARRIER_SRC_BYREF,
                                              2 * TARGET_POINTER_SIZE, INS_OPTS_POST_INDEX);
                        emit->emitIns_R_R_R_I(INS_stp, EA_8BYTE, tmpReg, tmpReg2, REG_WRITE_BARRIER_DST_BYREF,
                                              2 * TARGET_POINTER_SIZE, INS_OPTS_POST_INDEX);
                        ++i; // extra increment of i, since we are copying two items
                    }
                    else
                    {
                        emit->emitIns_R_R_I(INS_ldr, EA_8BYTE, tmpReg, REG_WRITE_BARRIER_SRC_BYREF, TARGET_POINTER_SIZE,
                                            INS_OPTS_POST_INDEX);
                        emit->emitIns_R_R_I(INS_str, EA_8BYTE, tmpReg, REG_WRITE_BARRIER_DST_BYREF, TARGET_POINTER_SIZE,
                                            INS_OPTS_POST_INDEX);
                    }
                    break;

                default:
                    // In the case of a GC-Pointer we'll call the ByRef write barrier helper
                    genEmitHelperCall(CORINFO_HELP_ASSIGN_BYREF, 0, EA_PTRSIZE);

                    gcPtrCount--;
                    break;
            }
            ++i;
        }
        assert(gcPtrCount == 0);
    }

    if (cpObjNode->gtFlags & GTF_BLK_VOLATILE)
    {
        // issue a INS_BARRIER_ISHLD after a volatile CpObj operation
        instGen_MemoryBarrier(INS_BARRIER_ISHLD);
    }

    // Clear the gcInfo for REG_WRITE_BARRIER_SRC_BYREF and REG_WRITE_BARRIER_DST_BYREF.
    // While we normally update GC info prior to the last instruction that uses them,
    // these actually live into the helper call.
    gcInfo.gcMarkRegSetNpt(RBM_WRITE_BARRIER_SRC_BYREF | RBM_WRITE_BARRIER_DST_BYREF);
}

// generate code do a switch statement based on a table of ip-relative offsets
void CodeGen::genTableBasedSwitch(GenTree* treeNode)
{
    genConsumeOperands(treeNode->AsOp());
    regNumber idxReg  = treeNode->gtOp.gtOp1->gtRegNum;
    regNumber baseReg = treeNode->gtOp.gtOp2->gtRegNum;

    regNumber tmpReg = treeNode->GetSingleTempReg();

    // load the ip-relative offset (which is relative to start of fgFirstBB)
    getEmitter()->emitIns_R_R_R(INS_ldr, EA_4BYTE, baseReg, baseReg, idxReg, INS_OPTS_LSL);

    // add it to the absolute address of fgFirstBB
    compiler->fgFirstBB->bbFlags |= BBF_JMP_TARGET;
    getEmitter()->emitIns_R_L(INS_adr, EA_PTRSIZE, compiler->fgFirstBB, tmpReg);
    getEmitter()->emitIns_R_R_R(INS_add, EA_PTRSIZE, baseReg, baseReg, tmpReg);

    // br baseReg
    getEmitter()->emitIns_R(INS_br, emitActualTypeSize(TYP_I_IMPL), baseReg);
}

// emits the table and an instruction to get the address of the first element
void CodeGen::genJumpTable(GenTree* treeNode)
{
    noway_assert(compiler->compCurBB->bbJumpKind == BBJ_SWITCH);
    assert(treeNode->OperGet() == GT_JMPTABLE);

    unsigned     jumpCount = compiler->compCurBB->bbJumpSwt->bbsCount;
    BasicBlock** jumpTable = compiler->compCurBB->bbJumpSwt->bbsDstTab;
    unsigned     jmpTabOffs;
    unsigned     jmpTabBase;

    jmpTabBase = getEmitter()->emitBBTableDataGenBeg(jumpCount, true);

    jmpTabOffs = 0;

    JITDUMP("\n      J_M%03u_DS%02u LABEL   DWORD\n", Compiler::s_compMethodsCount, jmpTabBase);

    for (unsigned i = 0; i < jumpCount; i++)
    {
        BasicBlock* target = *jumpTable++;
        noway_assert(target->bbFlags & BBF_JMP_TARGET);

        JITDUMP("            DD      L_M%03u_" FMT_BB "\n", Compiler::s_compMethodsCount, target->bbNum);

        getEmitter()->emitDataGenData(i, target);
    };

    getEmitter()->emitDataGenEnd();

    // Access to inline data is 'abstracted' by a special type of static member
    // (produced by eeFindJitDataOffs) which the emitter recognizes as being a reference
    // to constant data, not a real static field.
    getEmitter()->emitIns_R_C(INS_adr, emitActualTypeSize(TYP_I_IMPL), treeNode->gtRegNum, REG_NA,
                              compiler->eeFindJitDataOffs(jmpTabBase), 0);
    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genLockedInstructions: Generate code for a GT_XADD or GT_XCHG node.
//
// Arguments:
//    treeNode - the GT_XADD/XCHG node
//
void CodeGen::genLockedInstructions(GenTreeOp* treeNode)
{
    GenTree*  data      = treeNode->gtOp.gtOp2;
    GenTree*  addr      = treeNode->gtOp.gtOp1;
    regNumber targetReg = treeNode->gtRegNum;
    regNumber dataReg   = data->gtRegNum;
    regNumber addrReg   = addr->gtRegNum;

    genConsumeAddress(addr);
    genConsumeRegs(data);

    emitAttr dataSize = emitActualTypeSize(data);

    if (compiler->compSupports(InstructionSet_Atomics))
    {
        assert(!data->isContainedIntOrIImmed());

        switch (treeNode->gtOper)
        {
            case GT_XCHG:
                getEmitter()->emitIns_R_R_R(INS_swpal, dataSize, dataReg, targetReg, addrReg);
                break;
            case GT_XADD:
                if ((targetReg == REG_NA) || (targetReg == REG_ZR))
                {
                    getEmitter()->emitIns_R_R(INS_staddl, dataSize, dataReg, addrReg);
                }
                else
                {
                    getEmitter()->emitIns_R_R_R(INS_ldaddal, dataSize, dataReg, targetReg, addrReg);
                }
                break;
            default:
                assert(!"Unexpected treeNode->gtOper");
        }

        instGen_MemoryBarrier(INS_BARRIER_ISH);
    }
    else
    {
        regNumber exResultReg  = treeNode->ExtractTempReg(RBM_ALLINT);
        regNumber storeDataReg = (treeNode->OperGet() == GT_XCHG) ? dataReg : treeNode->ExtractTempReg(RBM_ALLINT);
        regNumber loadReg      = (targetReg != REG_NA) ? targetReg : storeDataReg;

        // Check allocator assumptions
        //
        // The register allocator should have extended the lifetimes of all input and internal registers so that
        // none interfere with the target.
        noway_assert(addrReg != targetReg);

        noway_assert(addrReg != loadReg);
        noway_assert(dataReg != loadReg);

        noway_assert(addrReg != storeDataReg);
        noway_assert((treeNode->OperGet() == GT_XCHG) || (addrReg != dataReg));

        assert(addr->isUsedFromReg());
        noway_assert(exResultReg != REG_NA);
        noway_assert(exResultReg != targetReg);
        noway_assert((targetReg != REG_NA) || (treeNode->OperGet() != GT_XCHG));

        // Store exclusive unpredictable cases must be avoided
        noway_assert(exResultReg != storeDataReg);
        noway_assert(exResultReg != addrReg);

        // NOTE: `genConsumeAddress` marks the consumed register as not a GC pointer, as it assumes that the input
        // registers
        // die at the first instruction generated by the node. This is not the case for these atomics as the  input
        // registers are multiply-used. As such, we need to mark the addr register as containing a GC pointer until
        // we are finished generating the code for this node.

        gcInfo.gcMarkRegPtrVal(addrReg, addr->TypeGet());

        // Emit code like this:
        //   retry:
        //     ldxr loadReg, [addrReg]
        //     add storeDataReg, loadReg, dataReg         # Only for GT_XADD
        //                                                # GT_XCHG storeDataReg === dataReg
        //     stxr exResult, storeDataReg, [addrReg]
        //     cbnz exResult, retry
        //     dmb ish

        BasicBlock* labelRetry = genCreateTempLabel();
        genDefineTempLabel(labelRetry);

        // The following instruction includes a acquire half barrier
        getEmitter()->emitIns_R_R(INS_ldaxr, dataSize, loadReg, addrReg);

        switch (treeNode->OperGet())
        {
            case GT_XADD:
                if (data->isContainedIntOrIImmed())
                {
                    // Even though INS_add is specified here, the encoder will choose either
                    // an INS_add or an INS_sub and encode the immediate as a positive value
                    genInstrWithConstant(INS_add, dataSize, storeDataReg, loadReg, data->AsIntConCommon()->IconValue(),
                                         REG_NA);
                }
                else
                {
                    getEmitter()->emitIns_R_R_R(INS_add, dataSize, storeDataReg, loadReg, dataReg);
                }
                break;
            case GT_XCHG:
                assert(!data->isContained());
                storeDataReg = dataReg;
                break;
            default:
                unreached();
        }

        // The following instruction includes a release half barrier
        getEmitter()->emitIns_R_R_R(INS_stlxr, dataSize, exResultReg, storeDataReg, addrReg);

        getEmitter()->emitIns_J_R(INS_cbnz, EA_4BYTE, labelRetry, exResultReg);

        instGen_MemoryBarrier(INS_BARRIER_ISH);

        gcInfo.gcMarkRegSetNpt(addr->gtGetRegMask());
    }

    if (treeNode->gtRegNum != REG_NA)
    {
        genProduceReg(treeNode);
    }
}

//------------------------------------------------------------------------
// genCodeForCmpXchg: Produce code for a GT_CMPXCHG node.
//
// Arguments:
//    tree - the GT_CMPXCHG node
//
void CodeGen::genCodeForCmpXchg(GenTreeCmpXchg* treeNode)
{
    assert(treeNode->OperIs(GT_CMPXCHG));

    GenTree* addr      = treeNode->gtOpLocation;  // arg1
    GenTree* data      = treeNode->gtOpValue;     // arg2
    GenTree* comparand = treeNode->gtOpComparand; // arg3

    regNumber targetReg    = treeNode->gtRegNum;
    regNumber dataReg      = data->gtRegNum;
    regNumber addrReg      = addr->gtRegNum;
    regNumber comparandReg = comparand->gtRegNum;

    genConsumeAddress(addr);
    genConsumeRegs(data);
    genConsumeRegs(comparand);

    if (compiler->compSupports(InstructionSet_Atomics))
    {
        emitAttr dataSize = emitActualTypeSize(data);

        // casal use the comparand as the target reg
        if (targetReg != comparandReg)
        {
            getEmitter()->emitIns_R_R(INS_mov, dataSize, targetReg, comparandReg);

            // Catch case we destroyed data or address before use
            noway_assert(addrReg != targetReg);
            noway_assert(dataReg != targetReg);
        }
        getEmitter()->emitIns_R_R_R(INS_casal, dataSize, targetReg, dataReg, addrReg);

        instGen_MemoryBarrier(INS_BARRIER_ISH);
    }
    else
    {
        regNumber exResultReg = treeNode->ExtractTempReg(RBM_ALLINT);

        // Check allocator assumptions
        //
        // The register allocator should have extended the lifetimes of all input and internal registers so that
        // none interfere with the target.
        noway_assert(addrReg != targetReg);
        noway_assert(dataReg != targetReg);
        noway_assert(comparandReg != targetReg);
        noway_assert(addrReg != dataReg);
        noway_assert(targetReg != REG_NA);
        noway_assert(exResultReg != REG_NA);
        noway_assert(exResultReg != targetReg);

        assert(addr->isUsedFromReg());
        assert(data->isUsedFromReg());
        assert(!comparand->isUsedFromMemory());

        // Store exclusive unpredictable cases must be avoided
        noway_assert(exResultReg != dataReg);
        noway_assert(exResultReg != addrReg);

        // NOTE: `genConsumeAddress` marks the consumed register as not a GC pointer, as it assumes that the input
        // registers
        // die at the first instruction generated by the node. This is not the case for these atomics as the  input
        // registers are multiply-used. As such, we need to mark the addr register as containing a GC pointer until
        // we are finished generating the code for this node.

        gcInfo.gcMarkRegPtrVal(addrReg, addr->TypeGet());

        // TODO-ARM64-CQ Use ARMv8.1 atomics if available
        // https://github.com/dotnet/coreclr/issues/11881

        // Emit code like this:
        //   retry:
        //     ldxr targetReg, [addrReg]
        //     cmp targetReg, comparandReg
        //     bne compareFail
        //     stxr exResult, dataReg, [addrReg]
        //     cbnz exResult, retry
        //   compareFail:
        //     dmb ish

        BasicBlock* labelRetry       = genCreateTempLabel();
        BasicBlock* labelCompareFail = genCreateTempLabel();
        genDefineTempLabel(labelRetry);

        // The following instruction includes a acquire half barrier
        getEmitter()->emitIns_R_R(INS_ldaxr, emitTypeSize(treeNode), targetReg, addrReg);

        if (comparand->isContainedIntOrIImmed())
        {
            if (comparand->IsIntegralConst(0))
            {
                getEmitter()->emitIns_J_R(INS_cbnz, emitActualTypeSize(treeNode), labelCompareFail, targetReg);
            }
            else
            {
                getEmitter()->emitIns_R_I(INS_cmp, emitActualTypeSize(treeNode), targetReg,
                                          comparand->AsIntConCommon()->IconValue());
                getEmitter()->emitIns_J(INS_bne, labelCompareFail);
            }
        }
        else
        {
            getEmitter()->emitIns_R_R(INS_cmp, emitActualTypeSize(treeNode), targetReg, comparandReg);
            getEmitter()->emitIns_J(INS_bne, labelCompareFail);
        }

        // The following instruction includes a release half barrier
        getEmitter()->emitIns_R_R_R(INS_stlxr, emitTypeSize(treeNode), exResultReg, dataReg, addrReg);

        getEmitter()->emitIns_J_R(INS_cbnz, EA_4BYTE, labelRetry, exResultReg);

        genDefineTempLabel(labelCompareFail);

        instGen_MemoryBarrier(INS_BARRIER_ISH);

        gcInfo.gcMarkRegSetNpt(addr->gtGetRegMask());
    }

    genProduceReg(treeNode);
}

instruction CodeGen::genGetInsForOper(genTreeOps oper, var_types type)
{
    instruction ins = INS_brk;

    if (varTypeIsFloating(type))
    {
        switch (oper)
        {
            case GT_ADD:
                ins = INS_fadd;
                break;
            case GT_SUB:
                ins = INS_fsub;
                break;
            case GT_MUL:
                ins = INS_fmul;
                break;
            case GT_DIV:
                ins = INS_fdiv;
                break;
            case GT_NEG:
                ins = INS_fneg;
                break;

            default:
                NYI("Unhandled oper in genGetInsForOper() - float");
                unreached();
                break;
        }
    }
    else
    {
        switch (oper)
        {
            case GT_ADD:
                ins = INS_add;
                break;
            case GT_AND:
                ins = INS_and;
                break;
            case GT_DIV:
                ins = INS_sdiv;
                break;
            case GT_UDIV:
                ins = INS_udiv;
                break;
            case GT_MUL:
                ins = INS_mul;
                break;
            case GT_LSH:
                ins = INS_lsl;
                break;
            case GT_NEG:
                ins = INS_neg;
                break;
            case GT_NOT:
                ins = INS_mvn;
                break;
            case GT_OR:
                ins = INS_orr;
                break;
            case GT_ROR:
                ins = INS_ror;
                break;
            case GT_RSH:
                ins = INS_asr;
                break;
            case GT_RSZ:
                ins = INS_lsr;
                break;
            case GT_SUB:
                ins = INS_sub;
                break;
            case GT_XOR:
                ins = INS_eor;
                break;

            default:
                NYI("Unhandled oper in genGetInsForOper() - integer");
                unreached();
                break;
        }
    }
    return ins;
}

//------------------------------------------------------------------------
// genCodeForReturnTrap: Produce code for a GT_RETURNTRAP node.
//
// Arguments:
//    tree - the GT_RETURNTRAP node
//
void CodeGen::genCodeForReturnTrap(GenTreeOp* tree)
{
    assert(tree->OperGet() == GT_RETURNTRAP);

    // this is nothing but a conditional call to CORINFO_HELP_STOP_FOR_GC
    // based on the contents of 'data'

    GenTree* data = tree->gtOp1;
    genConsumeRegs(data);
    getEmitter()->emitIns_R_I(INS_cmp, EA_4BYTE, data->gtRegNum, 0);

    BasicBlock* skipLabel = genCreateTempLabel();

    inst_JMP(EJ_eq, skipLabel);
    // emit the call to the EE-helper that stops for GC (or other reasons)

    genEmitHelperCall(CORINFO_HELP_STOP_FOR_GC, 0, EA_UNKNOWN);
    genDefineTempLabel(skipLabel);
}

//------------------------------------------------------------------------
// genCodeForStoreInd: Produce code for a GT_STOREIND node.
//
// Arguments:
//    tree - the GT_STOREIND node
//
void CodeGen::genCodeForStoreInd(GenTreeStoreInd* tree)
{
    GenTree*    data       = tree->Data();
    GenTree*    addr       = tree->Addr();
    var_types   targetType = tree->TypeGet();
    emitter*    emit       = getEmitter();
    emitAttr    attr       = emitTypeSize(tree);
    instruction ins        = ins_Store(targetType);

#ifdef FEATURE_SIMD
    // Storing Vector3 of size 12 bytes through indirection
    if (tree->TypeGet() == TYP_SIMD12)
    {
        genStoreIndTypeSIMD12(tree);
        return;
    }
#endif // FEATURE_SIMD

    GCInfo::WriteBarrierForm writeBarrierForm = gcInfo.gcIsWriteBarrierCandidate(tree, data);
    if (writeBarrierForm != GCInfo::WBF_NoBarrier)
    {
        // data and addr must be in registers.
        // Consume both registers so that any copies of interfering
        // registers are taken care of.
        genConsumeOperands(tree);

        // At this point, we should not have any interference.
        // That is, 'data' must not be in REG_WRITE_BARRIER_DST_BYREF,
        //  as that is where 'addr' must go.
        noway_assert(data->gtRegNum != REG_WRITE_BARRIER_DST_BYREF);

        // 'addr' goes into x14 (REG_WRITE_BARRIER_DST)
        genCopyRegIfNeeded(addr, REG_WRITE_BARRIER_DST);

        // 'data' goes into x15 (REG_WRITE_BARRIER_SRC)
        genCopyRegIfNeeded(data, REG_WRITE_BARRIER_SRC);

        genGCWriteBarrier(tree, writeBarrierForm);
    }
    else // A normal store, not a WriteBarrier store
    {
        bool     dataIsUnary = false;
        GenTree* nonRMWsrc   = nullptr;
        // We must consume the operands in the proper execution order,
        // so that liveness is updated appropriately.
        genConsumeAddress(addr);

        if (!data->isContained())
        {
            genConsumeRegs(data);
        }

        regNumber dataReg = REG_NA;
        if (data->isContainedIntOrIImmed())
        {
            assert(data->IsIntegralConst(0));
            dataReg = REG_ZR;
        }
        else // data is not contained, so evaluate it into a register
        {
            assert(!data->isContained());
            dataReg = data->gtRegNum;
        }

        assert((attr != EA_1BYTE) || !(tree->gtFlags & GTF_IND_UNALIGNED));

        if (tree->gtFlags & GTF_IND_VOLATILE)
        {
            bool useStoreRelease =
                genIsValidIntReg(dataReg) && !addr->isContained() && !(tree->gtFlags & GTF_IND_UNALIGNED);

            if (useStoreRelease)
            {
                switch (EA_SIZE(attr))
                {
                    case EA_1BYTE:
                        assert(ins == INS_strb);
                        ins = INS_stlrb;
                        break;
                    case EA_2BYTE:
                        assert(ins == INS_strh);
                        ins = INS_stlrh;
                        break;
                    case EA_4BYTE:
                    case EA_8BYTE:
                        assert(ins == INS_str);
                        ins = INS_stlr;
                        break;
                    default:
                        assert(false); // We should not get here
                }
            }
            else
            {
                // issue a full memory barrier before a volatile StInd
                instGen_MemoryBarrier();
            }
        }

        emit->emitInsLoadStoreOp(ins, attr, dataReg, tree);
    }
}

//------------------------------------------------------------------------
// genCodeForSwap: Produce code for a GT_SWAP node.
//
// Arguments:
//    tree - the GT_SWAP node
//
void CodeGen::genCodeForSwap(GenTreeOp* tree)
{
    assert(tree->OperIs(GT_SWAP));

    // Swap is only supported for lclVar operands that are enregistered
    // We do not consume or produce any registers.  Both operands remain enregistered.
    // However, the gc-ness may change.
    assert(genIsRegCandidateLocal(tree->gtOp1) && genIsRegCandidateLocal(tree->gtOp2));

    GenTreeLclVarCommon* lcl1    = tree->gtOp1->AsLclVarCommon();
    LclVarDsc*           varDsc1 = &(compiler->lvaTable[lcl1->gtLclNum]);
    var_types            type1   = varDsc1->TypeGet();
    GenTreeLclVarCommon* lcl2    = tree->gtOp2->AsLclVarCommon();
    LclVarDsc*           varDsc2 = &(compiler->lvaTable[lcl2->gtLclNum]);
    var_types            type2   = varDsc2->TypeGet();

    // We must have both int or both fp regs
    assert(!varTypeIsFloating(type1) || varTypeIsFloating(type2));

    // FP swap is not yet implemented (and should have NYI'd in LSRA)
    assert(!varTypeIsFloating(type1));

    regNumber oldOp1Reg     = lcl1->gtRegNum;
    regMaskTP oldOp1RegMask = genRegMask(oldOp1Reg);
    regNumber oldOp2Reg     = lcl2->gtRegNum;
    regMaskTP oldOp2RegMask = genRegMask(oldOp2Reg);

    // We don't call genUpdateVarReg because we don't have a tree node with the new register.
    varDsc1->lvRegNum = oldOp2Reg;
    varDsc2->lvRegNum = oldOp1Reg;

    // Do the xchg
    emitAttr size = EA_PTRSIZE;
    if (varTypeGCtype(type1) != varTypeGCtype(type2))
    {
        // If the type specified to the emitter is a GC type, it will swap the GC-ness of the registers.
        // Otherwise it will leave them alone, which is correct if they have the same GC-ness.
        size = EA_GCREF;
    }

    NYI("register swap");
    // inst_RV_RV(INS_xchg, oldOp1Reg, oldOp2Reg, TYP_I_IMPL, size);

    // Update the gcInfo.
    // Manually remove these regs for the gc sets (mostly to avoid confusing duplicative dump output)
    gcInfo.gcRegByrefSetCur &= ~(oldOp1RegMask | oldOp2RegMask);
    gcInfo.gcRegGCrefSetCur &= ~(oldOp1RegMask | oldOp2RegMask);

    // gcMarkRegPtrVal will do the appropriate thing for non-gc types.
    // It will also dump the updates.
    gcInfo.gcMarkRegPtrVal(oldOp2Reg, type1);
    gcInfo.gcMarkRegPtrVal(oldOp1Reg, type2);
}

//------------------------------------------------------------------------
// genIntToFloatCast: Generate code to cast an int/long to float/double
//
// Arguments:
//    treeNode - The GT_CAST node
//
// Return Value:
//    None.
//
// Assumptions:
//    Cast is a non-overflow conversion.
//    The treeNode must have an assigned register.
//    SrcType= int32/uint32/int64/uint64 and DstType=float/double.
//
void CodeGen::genIntToFloatCast(GenTree* treeNode)
{
    // int type --> float/double conversions are always non-overflow ones
    assert(treeNode->OperGet() == GT_CAST);
    assert(!treeNode->gtOverflow());

    regNumber targetReg = treeNode->gtRegNum;
    assert(genIsValidFloatReg(targetReg));

    GenTree* op1 = treeNode->gtOp.gtOp1;
    assert(!op1->isContained());             // Cannot be contained
    assert(genIsValidIntReg(op1->gtRegNum)); // Must be a valid int reg.

    var_types dstType = treeNode->CastToType();
    var_types srcType = genActualType(op1->TypeGet());
    assert(!varTypeIsFloating(srcType) && varTypeIsFloating(dstType));

    // force the srcType to unsigned if GT_UNSIGNED flag is set
    if (treeNode->gtFlags & GTF_UNSIGNED)
    {
        srcType = genUnsignedType(srcType);
    }

    // We should never see a srcType whose size is neither EA_4BYTE or EA_8BYTE
    emitAttr srcSize = EA_ATTR(genTypeSize(srcType));
    noway_assert((srcSize == EA_4BYTE) || (srcSize == EA_8BYTE));

    instruction ins       = varTypeIsUnsigned(srcType) ? INS_ucvtf : INS_scvtf;
    insOpts     cvtOption = INS_OPTS_NONE; // invalid value

    if (dstType == TYP_DOUBLE)
    {
        if (srcSize == EA_4BYTE)
        {
            cvtOption = INS_OPTS_4BYTE_TO_D;
        }
        else
        {
            assert(srcSize == EA_8BYTE);
            cvtOption = INS_OPTS_8BYTE_TO_D;
        }
    }
    else
    {
        assert(dstType == TYP_FLOAT);
        if (srcSize == EA_4BYTE)
        {
            cvtOption = INS_OPTS_4BYTE_TO_S;
        }
        else
        {
            assert(srcSize == EA_8BYTE);
            cvtOption = INS_OPTS_8BYTE_TO_S;
        }
    }

    genConsumeOperands(treeNode->AsOp());

    getEmitter()->emitIns_R_R(ins, emitActualTypeSize(dstType), treeNode->gtRegNum, op1->gtRegNum, cvtOption);

    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genFloatToIntCast: Generate code to cast float/double to int/long
//
// Arguments:
//    treeNode - The GT_CAST node
//
// Return Value:
//    None.
//
// Assumptions:
//    Cast is a non-overflow conversion.
//    The treeNode must have an assigned register.
//    SrcType=float/double and DstType= int32/uint32/int64/uint64
//
void CodeGen::genFloatToIntCast(GenTree* treeNode)
{
    // we don't expect to see overflow detecting float/double --> int type conversions here
    // as they should have been converted into helper calls by front-end.
    assert(treeNode->OperGet() == GT_CAST);
    assert(!treeNode->gtOverflow());

    regNumber targetReg = treeNode->gtRegNum;
    assert(genIsValidIntReg(targetReg)); // Must be a valid int reg.

    GenTree* op1 = treeNode->gtOp.gtOp1;
    assert(!op1->isContained());               // Cannot be contained
    assert(genIsValidFloatReg(op1->gtRegNum)); // Must be a valid float reg.

    var_types dstType = treeNode->CastToType();
    var_types srcType = op1->TypeGet();
    assert(varTypeIsFloating(srcType) && !varTypeIsFloating(dstType));

    // We should never see a dstType whose size is neither EA_4BYTE or EA_8BYTE
    // For conversions to small types (byte/sbyte/int16/uint16) from float/double,
    // we expect the front-end or lowering phase to have generated two levels of cast.
    //
    emitAttr dstSize = EA_ATTR(genTypeSize(dstType));
    noway_assert((dstSize == EA_4BYTE) || (dstSize == EA_8BYTE));

    instruction ins       = INS_fcvtzs;    // default to sign converts
    insOpts     cvtOption = INS_OPTS_NONE; // invalid value

    if (varTypeIsUnsigned(dstType))
    {
        ins = INS_fcvtzu; // use unsigned converts
    }

    if (srcType == TYP_DOUBLE)
    {
        if (dstSize == EA_4BYTE)
        {
            cvtOption = INS_OPTS_D_TO_4BYTE;
        }
        else
        {
            assert(dstSize == EA_8BYTE);
            cvtOption = INS_OPTS_D_TO_8BYTE;
        }
    }
    else
    {
        assert(srcType == TYP_FLOAT);
        if (dstSize == EA_4BYTE)
        {
            cvtOption = INS_OPTS_S_TO_4BYTE;
        }
        else
        {
            assert(dstSize == EA_8BYTE);
            cvtOption = INS_OPTS_S_TO_8BYTE;
        }
    }

    genConsumeOperands(treeNode->AsOp());

    getEmitter()->emitIns_R_R(ins, dstSize, treeNode->gtRegNum, op1->gtRegNum, cvtOption);

    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genCkfinite: Generate code for ckfinite opcode.
//
// Arguments:
//    treeNode - The GT_CKFINITE node
//
// Return Value:
//    None.
//
// Assumptions:
//    GT_CKFINITE node has reserved an internal register.
//
void CodeGen::genCkfinite(GenTree* treeNode)
{
    assert(treeNode->OperGet() == GT_CKFINITE);

    GenTree*  op1         = treeNode->gtOp.gtOp1;
    var_types targetType  = treeNode->TypeGet();
    int       expMask     = (targetType == TYP_FLOAT) ? 0x7F8 : 0x7FF; // Bit mask to extract exponent.
    int       shiftAmount = targetType == TYP_FLOAT ? 20 : 52;

    emitter* emit = getEmitter();

    // Extract exponent into a register.
    regNumber intReg = treeNode->GetSingleTempReg();
    regNumber fpReg  = genConsumeReg(op1);

    emit->emitIns_R_R(ins_Copy(targetType), emitActualTypeSize(treeNode), intReg, fpReg);
    emit->emitIns_R_R_I(INS_lsr, emitActualTypeSize(targetType), intReg, intReg, shiftAmount);

    // Mask of exponent with all 1's and check if the exponent is all 1's
    emit->emitIns_R_R_I(INS_and, EA_4BYTE, intReg, intReg, expMask);
    emit->emitIns_R_I(INS_cmp, EA_4BYTE, intReg, expMask);

    // If exponent is all 1's, throw ArithmeticException
    genJumpToThrowHlpBlk(EJ_eq, SCK_ARITH_EXCPN);

    // if it is a finite value copy it to targetReg
    if (treeNode->gtRegNum != fpReg)
    {
        emit->emitIns_R_R(ins_Copy(targetType), emitActualTypeSize(treeNode), treeNode->gtRegNum, fpReg);
    }
    genProduceReg(treeNode);
}

//------------------------------------------------------------------------
// genCodeForCompare: Produce code for a GT_EQ/GT_NE/GT_LT/GT_LE/GT_GE/GT_GT/GT_TEST_EQ/GT_TEST_NE node.
//
// Arguments:
//    tree - the node
//
void CodeGen::genCodeForCompare(GenTreeOp* tree)
{
    regNumber targetReg = tree->gtRegNum;
    emitter*  emit      = getEmitter();

    GenTree*  op1     = tree->gtOp1;
    GenTree*  op2     = tree->gtOp2;
    var_types op1Type = genActualType(op1->TypeGet());
    var_types op2Type = genActualType(op2->TypeGet());

    assert(!op1->isUsedFromMemory());
    assert(!op2->isUsedFromMemory());

    genConsumeOperands(tree);

    emitAttr cmpSize = EA_ATTR(genTypeSize(op1Type));

    assert(genTypeSize(op1Type) == genTypeSize(op2Type));

    if (varTypeIsFloating(op1Type))
    {
        assert(varTypeIsFloating(op2Type));
        assert(!op1->isContained());
        assert(op1Type == op2Type);

        if (op2->IsIntegralConst(0))
        {
            assert(op2->isContained());
            emit->emitIns_R_F(INS_fcmp, cmpSize, op1->gtRegNum, 0.0);
        }
        else
        {
            assert(!op2->isContained());
            emit->emitIns_R_R(INS_fcmp, cmpSize, op1->gtRegNum, op2->gtRegNum);
        }
    }
    else
    {
        assert(!varTypeIsFloating(op2Type));
        // We don't support swapping op1 and op2 to generate cmp reg, imm
        assert(!op1->isContainedIntOrIImmed());

        instruction ins = tree->OperIs(GT_TEST_EQ, GT_TEST_NE) ? INS_tst : INS_cmp;

        if (op2->isContainedIntOrIImmed())
        {
            GenTreeIntConCommon* intConst = op2->AsIntConCommon();
            emit->emitIns_R_I(ins, cmpSize, op1->gtRegNum, intConst->IconValue());
        }
        else
        {
            emit->emitIns_R_R(ins, cmpSize, op1->gtRegNum, op2->gtRegNum);
        }
    }

    // Are we evaluating this into a register?
    if (targetReg != REG_NA)
    {
        inst_SETCC(GenCondition::FromRelop(tree), tree->TypeGet(), targetReg);
        genProduceReg(tree);
    }
}

//------------------------------------------------------------------------
// genCodeForJumpCompare: Generates code for jmpCompare statement.
//
// A GT_JCMP node is created when a comparison and conditional branch
// can be executed in a single instruction.
//
// Arm64 has a few instructions with this behavior.
//   - cbz/cbnz -- Compare and branch register zero/not zero
//   - tbz/tbnz -- Test and branch register bit zero/not zero
//
// The cbz/cbnz supports the normal +/- 1MB branch range for conditional branches
// The tbz/tbnz supports a  smaller +/- 32KB branch range
//
// A GT_JCMP cbz/cbnz node is created when there is a GT_EQ or GT_NE
// integer/unsigned comparison against #0 which is used by a GT_JTRUE
// condition jump node.
//
// A GT_JCMP tbz/tbnz node is created when there is a GT_TEST_EQ or GT_TEST_NE
// integer/unsigned comparison against against a mask with a single bit set
// which is used by a GT_JTRUE condition jump node.
//
// This node is repsonsible for consuming the register, and emitting the
// appropriate fused compare/test and branch instruction
//
// Two flags guide code generation
//    GTF_JCMP_TST -- Set if this is a tbz/tbnz rather than cbz/cbnz
//    GTF_JCMP_EQ  -- Set if this is cbz/tbz rather than cbnz/tbnz
//
// Arguments:
//    tree - The GT_JCMP tree node.
//
// Return Value:
//    None
//
void CodeGen::genCodeForJumpCompare(GenTreeOp* tree)
{
    assert(compiler->compCurBB->bbJumpKind == BBJ_COND);

    GenTree* op1 = tree->gtGetOp1();
    GenTree* op2 = tree->gtGetOp2();

    assert(tree->OperIs(GT_JCMP));
    assert(!varTypeIsFloating(tree));
    assert(!op1->isUsedFromMemory());
    assert(!op2->isUsedFromMemory());
    assert(op2->IsCnsIntOrI());
    assert(op2->isContained());

    genConsumeOperands(tree);

    regNumber reg  = op1->gtRegNum;
    emitAttr  attr = emitActualTypeSize(op1->TypeGet());

    if (tree->gtFlags & GTF_JCMP_TST)
    {
        ssize_t compareImm = op2->gtIntCon.IconValue();

        assert(isPow2(compareImm));

        instruction ins = (tree->gtFlags & GTF_JCMP_EQ) ? INS_tbz : INS_tbnz;
        int         imm = genLog2((size_t)compareImm);

        getEmitter()->emitIns_J_R_I(ins, attr, compiler->compCurBB->bbJumpDest, reg, imm);
    }
    else
    {
        assert(op2->IsIntegralConst(0));

        instruction ins = (tree->gtFlags & GTF_JCMP_EQ) ? INS_cbz : INS_cbnz;

        getEmitter()->emitIns_J_R(ins, attr, compiler->compCurBB->bbJumpDest, reg);
    }
}

//---------------------------------------------------------------------
// genSPtoFPdelta - return offset from the stack pointer (Initial-SP) to the frame pointer. The frame pointer
// will point to the saved frame pointer slot (i.e., there will be frame pointer chaining).
//
int CodeGenInterface::genSPtoFPdelta() const
{
    assert(isFramePointerUsed());
    int delta = -1; // initialization to illegal value

    if (IsSaveFpLrWithAllCalleeSavedRegisters())
    {
        // The saved frame pointer is at the top of the frame, just beneath the saved varargs register space and the
        // saved LR.
        delta = genTotalFrameSize() - (compiler->info.compIsVarArgs ? MAX_REG_ARG * REGSIZE_BYTES : 0) -
                2 /* FP, LR */ * REGSIZE_BYTES;
    }
    else
    {
        // We place the saved frame pointer immediately above the outgoing argument space.
        delta = (int)compiler->lvaOutgoingArgSpaceSize;
    }

    assert(delta >= 0);
    return delta;
}

//---------------------------------------------------------------------
// genTotalFrameSize - return the total size of the stack frame, including local size,
// callee-saved register size, etc.
//
// Return value:
//    Total frame size
//

int CodeGenInterface::genTotalFrameSize() const
{
    // For varargs functions, we home all the incoming register arguments. They are not
    // included in the compCalleeRegsPushed count. This is like prespill on ARM32, but
    // since we don't use "push" instructions to save them, we don't have to do the
    // save of these varargs register arguments as the first thing in the prolog.

    assert(!IsUninitialized(compiler->compCalleeRegsPushed));

    int totalFrameSize = (compiler->info.compIsVarArgs ? MAX_REG_ARG * REGSIZE_BYTES : 0) +
                         compiler->compCalleeRegsPushed * REGSIZE_BYTES + compiler->compLclFrameSize;

    assert(totalFrameSize >= 0);
    return totalFrameSize;
}

//---------------------------------------------------------------------
// genCallerSPtoFPdelta - return the offset from Caller-SP to the frame pointer.
// This number is going to be negative, since the Caller-SP is at a higher
// address than the frame pointer.
//
// There must be a frame pointer to call this function!

int CodeGenInterface::genCallerSPtoFPdelta() const
{
    assert(isFramePointerUsed());
    int callerSPtoFPdelta;

    callerSPtoFPdelta = genCallerSPtoInitialSPdelta() + genSPtoFPdelta();

    assert(callerSPtoFPdelta <= 0);
    return callerSPtoFPdelta;
}

//---------------------------------------------------------------------
// genCallerSPtoInitialSPdelta - return the offset from Caller-SP to Initial SP.
//
// This number will be negative.

int CodeGenInterface::genCallerSPtoInitialSPdelta() const
{
    int callerSPtoSPdelta = 0;

    callerSPtoSPdelta -= genTotalFrameSize();

    assert(callerSPtoSPdelta <= 0);
    return callerSPtoSPdelta;
}

//---------------------------------------------------------------------
// SetSaveFpLrWithAllCalleeSavedRegisters - Set the variable that indicates if FP/LR registers
// are stored with the rest of the callee-saved registers.
//
void CodeGen::SetSaveFpLrWithAllCalleeSavedRegisters(bool value)
{
    JITDUMP("Setting genSaveFpLrWithAllCalleeSavedRegisters to %s\n", dspBool(value));
    genSaveFpLrWithAllCalleeSavedRegisters = value;
}

//---------------------------------------------------------------------
// IsSaveFpLrWithAllCalleeSavedRegisters - Return the value that indicates where FP/LR registers
// are stored in the prolog.
//
bool CodeGen::IsSaveFpLrWithAllCalleeSavedRegisters() const
{
    return genSaveFpLrWithAllCalleeSavedRegisters;
}

/*****************************************************************************
 *  Emit a call to a helper function.
 *
 */

void CodeGen::genEmitHelperCall(unsigned helper, int argSize, emitAttr retSize, regNumber callTargetReg /*= REG_NA */)
{
    void* addr  = nullptr;
    void* pAddr = nullptr;

    emitter::EmitCallType callType = emitter::EC_FUNC_TOKEN;
    addr                           = compiler->compGetHelperFtn((CorInfoHelpFunc)helper, &pAddr);
    regNumber callTarget           = REG_NA;

    if (addr == nullptr)
    {
        // This is call to a runtime helper.
        // adrp x, [reloc:rel page addr]
        // add x, x, [reloc:page offset]
        // ldr x, [x]
        // br x

        if (callTargetReg == REG_NA)
        {
            // If a callTargetReg has not been explicitly provided, we will use REG_DEFAULT_HELPER_CALL_TARGET, but
            // this is only a valid assumption if the helper call is known to kill REG_DEFAULT_HELPER_CALL_TARGET.
            callTargetReg = REG_DEFAULT_HELPER_CALL_TARGET;
        }

        regMaskTP callTargetMask = genRegMask(callTargetReg);
        regMaskTP callKillSet    = compiler->compHelperCallKillSet((CorInfoHelpFunc)helper);

        // assert that all registers in callTargetMask are in the callKillSet
        noway_assert((callTargetMask & callKillSet) == callTargetMask);

        callTarget = callTargetReg;

        // adrp + add with relocations will be emitted
        getEmitter()->emitIns_R_AI(INS_adrp, EA_PTR_DSP_RELOC, callTarget, (ssize_t)pAddr);
        getEmitter()->emitIns_R_R(INS_ldr, EA_PTRSIZE, callTarget, callTarget);
        callType = emitter::EC_INDIR_R;
    }

    getEmitter()->emitIns_Call(callType, compiler->eeFindHelper(helper), INDEBUG_LDISASM_COMMA(nullptr) addr, argSize,
                               retSize, EA_UNKNOWN, gcInfo.gcVarPtrSetCur, gcInfo.gcRegGCrefSetCur,
                               gcInfo.gcRegByrefSetCur, BAD_IL_OFFSET, /* IL offset */
                               callTarget,                             /* ireg */
                               REG_NA, 0, 0,                           /* xreg, xmul, disp */
                               false                                   /* isJump */
                               );

    regMaskTP killMask = compiler->compHelperCallKillSet((CorInfoHelpFunc)helper);
    regSet.verifyRegistersUsed(killMask);
}

#ifdef FEATURE_SIMD

//------------------------------------------------------------------------
// genSIMDIntrinsic: Generate code for a SIMD Intrinsic.  This is the main
// routine which in turn calls appropriate genSIMDIntrinsicXXX() routine.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
// Notes:
//    Currently, we only recognize SIMDVector<float> and SIMDVector<int>, and
//    a limited set of methods.
//
// TODO-CLEANUP Merge all versions of this function and move to new file simdcodegencommon.cpp.
void CodeGen::genSIMDIntrinsic(GenTreeSIMD* simdNode)
{
    // NYI for unsupported base types
    if (simdNode->gtSIMDBaseType != TYP_INT && simdNode->gtSIMDBaseType != TYP_LONG &&
        simdNode->gtSIMDBaseType != TYP_FLOAT && simdNode->gtSIMDBaseType != TYP_DOUBLE &&
        simdNode->gtSIMDBaseType != TYP_USHORT && simdNode->gtSIMDBaseType != TYP_UBYTE &&
        simdNode->gtSIMDBaseType != TYP_SHORT && simdNode->gtSIMDBaseType != TYP_BYTE &&
        simdNode->gtSIMDBaseType != TYP_UINT && simdNode->gtSIMDBaseType != TYP_ULONG)
    {
        noway_assert(!"SIMD intrinsic with unsupported base type.");
    }

    switch (simdNode->gtSIMDIntrinsicID)
    {
        case SIMDIntrinsicInit:
            genSIMDIntrinsicInit(simdNode);
            break;

        case SIMDIntrinsicInitN:
            genSIMDIntrinsicInitN(simdNode);
            break;

        case SIMDIntrinsicSqrt:
        case SIMDIntrinsicAbs:
        case SIMDIntrinsicCast:
        case SIMDIntrinsicConvertToSingle:
        case SIMDIntrinsicConvertToInt32:
        case SIMDIntrinsicConvertToDouble:
        case SIMDIntrinsicConvertToInt64:
            genSIMDIntrinsicUnOp(simdNode);
            break;

        case SIMDIntrinsicWidenLo:
        case SIMDIntrinsicWidenHi:
            genSIMDIntrinsicWiden(simdNode);
            break;

        case SIMDIntrinsicNarrow:
            genSIMDIntrinsicNarrow(simdNode);
            break;

        case SIMDIntrinsicAdd:
        case SIMDIntrinsicSub:
        case SIMDIntrinsicMul:
        case SIMDIntrinsicDiv:
        case SIMDIntrinsicBitwiseAnd:
        case SIMDIntrinsicBitwiseAndNot:
        case SIMDIntrinsicBitwiseOr:
        case SIMDIntrinsicBitwiseXor:
        case SIMDIntrinsicMin:
        case SIMDIntrinsicMax:
        case SIMDIntrinsicEqual:
        case SIMDIntrinsicLessThan:
        case SIMDIntrinsicGreaterThan:
        case SIMDIntrinsicLessThanOrEqual:
        case SIMDIntrinsicGreaterThanOrEqual:
            genSIMDIntrinsicBinOp(simdNode);
            break;

        case SIMDIntrinsicOpEquality:
        case SIMDIntrinsicOpInEquality:
            genSIMDIntrinsicRelOp(simdNode);
            break;

        case SIMDIntrinsicDotProduct:
            genSIMDIntrinsicDotProduct(simdNode);
            break;

        case SIMDIntrinsicGetItem:
            genSIMDIntrinsicGetItem(simdNode);
            break;

        case SIMDIntrinsicSetX:
        case SIMDIntrinsicSetY:
        case SIMDIntrinsicSetZ:
        case SIMDIntrinsicSetW:
            genSIMDIntrinsicSetItem(simdNode);
            break;

        case SIMDIntrinsicUpperSave:
            genSIMDIntrinsicUpperSave(simdNode);
            break;

        case SIMDIntrinsicUpperRestore:
            genSIMDIntrinsicUpperRestore(simdNode);
            break;

        case SIMDIntrinsicSelect:
            NYI("SIMDIntrinsicSelect lowered during import to (a & sel) | (b & ~sel)");
            break;

        default:
            noway_assert(!"Unimplemented SIMD intrinsic.");
            unreached();
    }
}

insOpts CodeGen::genGetSimdInsOpt(emitAttr size, var_types elementType)
{
    assert((size == EA_16BYTE) || (size == EA_8BYTE));
    insOpts result = INS_OPTS_NONE;

    switch (elementType)
    {
        case TYP_DOUBLE:
        case TYP_ULONG:
        case TYP_LONG:
            result = (size == EA_16BYTE) ? INS_OPTS_2D : INS_OPTS_1D;
            break;
        case TYP_FLOAT:
        case TYP_UINT:
        case TYP_INT:
            result = (size == EA_16BYTE) ? INS_OPTS_4S : INS_OPTS_2S;
            break;
        case TYP_USHORT:
        case TYP_SHORT:
            result = (size == EA_16BYTE) ? INS_OPTS_8H : INS_OPTS_4H;
            break;
        case TYP_UBYTE:
        case TYP_BYTE:
            result = (size == EA_16BYTE) ? INS_OPTS_16B : INS_OPTS_8B;
            break;
        default:
            assert(!"Unsupported element type");
            unreached();
    }

    return result;
}

// getOpForSIMDIntrinsic: return the opcode for the given SIMD Intrinsic
//
// Arguments:
//   intrinsicId    -   SIMD intrinsic Id
//   baseType       -   Base type of the SIMD vector
//   immed          -   Out param. Any immediate byte operand that needs to be passed to SSE2 opcode
//
//
// Return Value:
//   Instruction (op) to be used, and immed is set if instruction requires an immediate operand.
//
instruction CodeGen::getOpForSIMDIntrinsic(SIMDIntrinsicID intrinsicId, var_types baseType, unsigned* ival /*=nullptr*/)
{
    instruction result = INS_invalid;
    if (varTypeIsFloating(baseType))
    {
        switch (intrinsicId)
        {
            case SIMDIntrinsicAbs:
                result = INS_fabs;
                break;
            case SIMDIntrinsicAdd:
                result = INS_fadd;
                break;
            case SIMDIntrinsicBitwiseAnd:
                result = INS_and;
                break;
            case SIMDIntrinsicBitwiseAndNot:
                result = INS_bic;
                break;
            case SIMDIntrinsicBitwiseOr:
                result = INS_orr;
                break;
            case SIMDIntrinsicBitwiseXor:
                result = INS_eor;
                break;
            case SIMDIntrinsicCast:
                result = INS_mov;
                break;
            case SIMDIntrinsicConvertToInt32:
            case SIMDIntrinsicConvertToInt64:
                result = INS_fcvtns;
                break;
            case SIMDIntrinsicDiv:
                result = INS_fdiv;
                break;
            case SIMDIntrinsicEqual:
                result = INS_fcmeq;
                break;
            case SIMDIntrinsicGreaterThan:
                result = INS_fcmgt;
                break;
            case SIMDIntrinsicGreaterThanOrEqual:
                result = INS_fcmge;
                break;
            case SIMDIntrinsicLessThan:
                result = INS_fcmlt;
                break;
            case SIMDIntrinsicLessThanOrEqual:
                result = INS_fcmle;
                break;
            case SIMDIntrinsicMax:
                result = INS_fmax;
                break;
            case SIMDIntrinsicMin:
                result = INS_fmin;
                break;
            case SIMDIntrinsicMul:
                result = INS_fmul;
                break;
            case SIMDIntrinsicNarrow:
                // Use INS_fcvtn lower bytes of result followed by INS_fcvtn2 for upper bytes
                // Return lower bytes instruction here
                result = INS_fcvtn;
                break;
            case SIMDIntrinsicSelect:
                result = INS_bsl;
                break;
            case SIMDIntrinsicSqrt:
                result = INS_fsqrt;
                break;
            case SIMDIntrinsicSub:
                result = INS_fsub;
                break;
            case SIMDIntrinsicWidenLo:
                result = INS_fcvtl;
                break;
            case SIMDIntrinsicWidenHi:
                result = INS_fcvtl2;
                break;
            default:
                assert(!"Unsupported SIMD intrinsic");
                unreached();
        }
    }
    else
    {
        bool isUnsigned = varTypeIsUnsigned(baseType);

        switch (intrinsicId)
        {
            case SIMDIntrinsicAbs:
                assert(!isUnsigned);
                result = INS_abs;
                break;
            case SIMDIntrinsicAdd:
                result = INS_add;
                break;
            case SIMDIntrinsicBitwiseAnd:
                result = INS_and;
                break;
            case SIMDIntrinsicBitwiseAndNot:
                result = INS_bic;
                break;
            case SIMDIntrinsicBitwiseOr:
                result = INS_orr;
                break;
            case SIMDIntrinsicBitwiseXor:
                result = INS_eor;
                break;
            case SIMDIntrinsicCast:
                result = INS_mov;
                break;
            case SIMDIntrinsicConvertToDouble:
            case SIMDIntrinsicConvertToSingle:
                result = isUnsigned ? INS_ucvtf : INS_scvtf;
                break;
            case SIMDIntrinsicEqual:
                result = INS_cmeq;
                break;
            case SIMDIntrinsicGreaterThan:
                result = isUnsigned ? INS_cmhi : INS_cmgt;
                break;
            case SIMDIntrinsicGreaterThanOrEqual:
                result = isUnsigned ? INS_cmhs : INS_cmge;
                break;
            case SIMDIntrinsicLessThan:
                assert(!isUnsigned);
                result = INS_cmlt;
                break;
            case SIMDIntrinsicLessThanOrEqual:
                assert(!isUnsigned);
                result = INS_cmle;
                break;
            case SIMDIntrinsicMax:
                result = isUnsigned ? INS_umax : INS_smax;
                break;
            case SIMDIntrinsicMin:
                result = isUnsigned ? INS_umin : INS_smin;
                break;
            case SIMDIntrinsicMul:
                result = INS_mul;
                break;
            case SIMDIntrinsicNarrow:
                // Use INS_xtn lower bytes of result followed by INS_xtn2 for upper bytes
                // Return lower bytes instruction here
                result = INS_xtn;
                break;
            case SIMDIntrinsicSelect:
                result = INS_bsl;
                break;
            case SIMDIntrinsicSub:
                result = INS_sub;
                break;
            case SIMDIntrinsicWidenLo:
                result = isUnsigned ? INS_uxtl : INS_sxtl;
                break;
            case SIMDIntrinsicWidenHi:
                result = isUnsigned ? INS_uxtl2 : INS_sxtl2;
                break;
            default:
                assert(!"Unsupported SIMD intrinsic");
                unreached();
        }
    }

    noway_assert(result != INS_invalid);
    return result;
}

//------------------------------------------------------------------------
// genSIMDIntrinsicInit: Generate code for SIMD Intrinsic Initialize.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicInit(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicInit);

    GenTree*  op1       = simdNode->gtGetOp1();
    var_types baseType  = simdNode->gtSIMDBaseType;
    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);
    var_types targetType = simdNode->TypeGet();

    genConsumeOperands(simdNode);
    regNumber op1Reg = op1->IsIntegralConst(0) ? REG_ZR : op1->gtRegNum;

    // TODO-ARM64-CQ Add LD1R to allow SIMDIntrinsicInit from contained memory
    // TODO-ARM64-CQ Add MOVI to allow SIMDIntrinsicInit from contained immediate small constants

    assert(op1->isContained() == op1->IsIntegralConst(0));
    assert(!op1->isUsedFromMemory());

    assert(genIsValidFloatReg(targetReg));
    assert(genIsValidIntReg(op1Reg) || genIsValidFloatReg(op1Reg));

    emitAttr attr = (simdNode->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts  opt  = genGetSimdInsOpt(attr, baseType);

    if (genIsValidIntReg(op1Reg))
    {
        getEmitter()->emitIns_R_R(INS_dup, attr, targetReg, op1Reg, opt);
    }
    else
    {
        getEmitter()->emitIns_R_R_I(INS_dup, attr, targetReg, op1Reg, 0, opt);
    }

    genProduceReg(simdNode);
}

//-------------------------------------------------------------------------------------------
// genSIMDIntrinsicInitN: Generate code for SIMD Intrinsic Initialize for the form that takes
//                        a number of arguments equal to the length of the Vector.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicInitN(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicInitN);

    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);

    var_types targetType = simdNode->TypeGet();

    var_types baseType = simdNode->gtSIMDBaseType;

    regNumber vectorReg = targetReg;

    if (varTypeIsFloating(baseType))
    {
        // Note that we cannot use targetReg before consuming all float source operands.
        // Therefore use an internal temp register
        vectorReg = simdNode->GetSingleTempReg(RBM_ALLFLOAT);
    }

    emitAttr baseTypeSize = emitTypeSize(baseType);

    // We will first consume the list items in execution (left to right) order,
    // and record the registers.
    regNumber operandRegs[FP_REGSIZE_BYTES];
    unsigned  initCount = 0;
    for (GenTree* list = simdNode->gtGetOp1(); list != nullptr; list = list->gtGetOp2())
    {
        assert(list->OperGet() == GT_LIST);
        GenTree* listItem = list->gtGetOp1();
        assert(listItem->TypeGet() == baseType);
        assert(!listItem->isContained());
        regNumber operandReg   = genConsumeReg(listItem);
        operandRegs[initCount] = operandReg;
        initCount++;
    }

    assert((initCount * baseTypeSize) <= simdNode->gtSIMDSize);

    if (initCount * baseTypeSize < EA_16BYTE)
    {
        getEmitter()->emitIns_R_I(INS_movi, EA_16BYTE, vectorReg, 0x00, INS_OPTS_16B);
    }

    if (varTypeIsIntegral(baseType))
    {
        for (unsigned i = 0; i < initCount; i++)
        {
            getEmitter()->emitIns_R_R_I(INS_ins, baseTypeSize, vectorReg, operandRegs[i], i);
        }
    }
    else
    {
        for (unsigned i = 0; i < initCount; i++)
        {
            getEmitter()->emitIns_R_R_I_I(INS_ins, baseTypeSize, vectorReg, operandRegs[i], i, 0);
        }
    }

    // Load the initialized value.
    if (targetReg != vectorReg)
    {
        getEmitter()->emitIns_R_R(INS_mov, EA_16BYTE, targetReg, vectorReg);
    }

    genProduceReg(simdNode);
}

//----------------------------------------------------------------------------------
// genSIMDIntrinsicUnOp: Generate code for SIMD Intrinsic unary operations like sqrt.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicUnOp(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicSqrt || simdNode->gtSIMDIntrinsicID == SIMDIntrinsicCast ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicAbs ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicConvertToSingle ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicConvertToInt32 ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicConvertToDouble ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicConvertToInt64);

    GenTree*  op1       = simdNode->gtGetOp1();
    var_types baseType  = simdNode->gtSIMDBaseType;
    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);
    var_types targetType = simdNode->TypeGet();

    genConsumeOperands(simdNode);
    regNumber op1Reg = op1->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(targetReg));

    instruction ins  = getOpForSIMDIntrinsic(simdNode->gtSIMDIntrinsicID, baseType);
    emitAttr    attr = (simdNode->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = (ins == INS_mov) ? INS_OPTS_NONE : genGetSimdInsOpt(attr, baseType);

    getEmitter()->emitIns_R_R(ins, attr, targetReg, op1Reg, opt);

    genProduceReg(simdNode);
}

//--------------------------------------------------------------------------------
// genSIMDIntrinsicWiden: Generate code for SIMD Intrinsic Widen operations
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Notes:
//    The Widen intrinsics are broken into separate intrinsics for the two results.
//
void CodeGen::genSIMDIntrinsicWiden(GenTreeSIMD* simdNode)
{
    assert((simdNode->gtSIMDIntrinsicID == SIMDIntrinsicWidenLo) ||
           (simdNode->gtSIMDIntrinsicID == SIMDIntrinsicWidenHi));

    GenTree*  op1       = simdNode->gtGetOp1();
    var_types baseType  = simdNode->gtSIMDBaseType;
    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);
    var_types simdType = simdNode->TypeGet();

    genConsumeOperands(simdNode);
    regNumber op1Reg   = op1->gtRegNum;
    regNumber srcReg   = op1Reg;
    emitAttr  emitSize = emitActualTypeSize(simdType);

    instruction ins = getOpForSIMDIntrinsic(simdNode->gtSIMDIntrinsicID, baseType);

    if (varTypeIsFloating(baseType))
    {
        getEmitter()->emitIns_R_R(ins, EA_8BYTE, targetReg, op1Reg);
    }
    else
    {
        emitAttr attr = (simdNode->gtSIMDIntrinsicID == SIMDIntrinsicWidenHi) ? EA_16BYTE : EA_8BYTE;
        insOpts  opt  = genGetSimdInsOpt(attr, baseType);

        getEmitter()->emitIns_R_R(ins, attr, targetReg, op1Reg, opt);
    }

    genProduceReg(simdNode);
}

//--------------------------------------------------------------------------------
// genSIMDIntrinsicNarrow: Generate code for SIMD Intrinsic Narrow operations
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Notes:
//    This intrinsic takes two arguments. The first operand is narrowed to produce the
//    lower elements of the results, and the second operand produces the high elements.
//
void CodeGen::genSIMDIntrinsicNarrow(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicNarrow);

    GenTree*  op1       = simdNode->gtGetOp1();
    GenTree*  op2       = simdNode->gtGetOp2();
    var_types baseType  = simdNode->gtSIMDBaseType;
    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);
    var_types simdType = simdNode->TypeGet();
    emitAttr  emitSize = emitTypeSize(simdType);

    genConsumeOperands(simdNode);
    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(op2Reg));
    assert(genIsValidFloatReg(targetReg));
    assert(op2Reg != targetReg);
    assert(simdNode->gtSIMDSize == 16);

    instruction ins = getOpForSIMDIntrinsic(simdNode->gtSIMDIntrinsicID, baseType);
    assert((ins == INS_fcvtn) || (ins == INS_xtn));

    if (ins == INS_fcvtn)
    {
        getEmitter()->emitIns_R_R(INS_fcvtn, EA_8BYTE, targetReg, op1Reg);
        getEmitter()->emitIns_R_R(INS_fcvtn2, EA_8BYTE, targetReg, op2Reg);
    }
    else
    {
        insOpts opt  = INS_OPTS_NONE;
        insOpts opt2 = INS_OPTS_NONE;

        // This is not the same as genGetSimdInsOpt()
        // Basetype is the soure operand type
        // However encoding is based on the destination operand type which is 1/2 the basetype.
        switch (baseType)
        {
            case TYP_ULONG:
            case TYP_LONG:
                opt  = INS_OPTS_2S;
                opt2 = INS_OPTS_4S;
                break;
            case TYP_UINT:
            case TYP_INT:
                opt  = INS_OPTS_4H;
                opt2 = INS_OPTS_8H;
                break;
            case TYP_USHORT:
            case TYP_SHORT:
                opt  = INS_OPTS_8B;
                opt2 = INS_OPTS_16B;
                break;
            default:
                assert(!"Unsupported narrowing element type");
                unreached();
        }
        getEmitter()->emitIns_R_R(INS_xtn, EA_8BYTE, targetReg, op1Reg, opt);
        getEmitter()->emitIns_R_R(INS_xtn2, EA_16BYTE, targetReg, op2Reg, opt2);
    }

    genProduceReg(simdNode);
}

//--------------------------------------------------------------------------------
// genSIMDIntrinsicBinOp: Generate code for SIMD Intrinsic binary operations
// add, sub, mul, bit-wise And, AndNot and Or.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicBinOp(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicAdd || simdNode->gtSIMDIntrinsicID == SIMDIntrinsicSub ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicMul || simdNode->gtSIMDIntrinsicID == SIMDIntrinsicDiv ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicBitwiseAnd ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicBitwiseAndNot ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicBitwiseOr ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicBitwiseXor || simdNode->gtSIMDIntrinsicID == SIMDIntrinsicMin ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicMax || simdNode->gtSIMDIntrinsicID == SIMDIntrinsicEqual ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicLessThan ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicGreaterThan ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicLessThanOrEqual ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicGreaterThanOrEqual);

    GenTree*  op1       = simdNode->gtGetOp1();
    GenTree*  op2       = simdNode->gtGetOp2();
    var_types baseType  = simdNode->gtSIMDBaseType;
    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);
    var_types targetType = simdNode->TypeGet();

    genConsumeOperands(simdNode);
    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(op2Reg));
    assert(genIsValidFloatReg(targetReg));

    // TODO-ARM64-CQ Contain integer constants where posible

    instruction ins  = getOpForSIMDIntrinsic(simdNode->gtSIMDIntrinsicID, baseType);
    emitAttr    attr = (simdNode->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = genGetSimdInsOpt(attr, baseType);

    getEmitter()->emitIns_R_R_R(ins, attr, targetReg, op1Reg, op2Reg, opt);

    genProduceReg(simdNode);
}

//--------------------------------------------------------------------------------
// genSIMDIntrinsicRelOp: Generate code for a SIMD Intrinsic relational operater
// == and !=
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicRelOp(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicOpEquality ||
           simdNode->gtSIMDIntrinsicID == SIMDIntrinsicOpInEquality);

    GenTree*  op1        = simdNode->gtGetOp1();
    GenTree*  op2        = simdNode->gtGetOp2();
    var_types baseType   = simdNode->gtSIMDBaseType;
    regNumber targetReg  = simdNode->gtRegNum;
    var_types targetType = simdNode->TypeGet();

    genConsumeOperands(simdNode);
    regNumber op1Reg   = op1->gtRegNum;
    regNumber op2Reg   = op2->gtRegNum;
    regNumber otherReg = op2Reg;

    instruction ins  = getOpForSIMDIntrinsic(SIMDIntrinsicEqual, baseType);
    emitAttr    attr = (simdNode->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = genGetSimdInsOpt(attr, baseType);

    // TODO-ARM64-CQ Contain integer constants where possible

    regNumber tmpFloatReg = simdNode->GetSingleTempReg(RBM_ALLFLOAT);

    getEmitter()->emitIns_R_R_R(ins, attr, tmpFloatReg, op1Reg, op2Reg, opt);

    if ((simdNode->gtFlags & GTF_SIMD12_OP) != 0)
    {
        // For 12Byte vectors we must set upper bits to get correct comparison
        // We do not assume upper bits are zero.
        instGen_Set_Reg_To_Imm(EA_4BYTE, targetReg, -1);
        getEmitter()->emitIns_R_R_I(INS_ins, EA_4BYTE, tmpFloatReg, targetReg, 3);
    }

    getEmitter()->emitIns_R_R(INS_uminv, attr, tmpFloatReg, tmpFloatReg,
                              (simdNode->gtSIMDSize > 8) ? INS_OPTS_16B : INS_OPTS_8B);

    getEmitter()->emitIns_R_R_I(INS_mov, EA_1BYTE, targetReg, tmpFloatReg, 0);

    if (simdNode->gtSIMDIntrinsicID == SIMDIntrinsicOpInEquality)
    {
        getEmitter()->emitIns_R_R_I(INS_eor, EA_4BYTE, targetReg, targetReg, 0x1);
    }

    getEmitter()->emitIns_R_R_I(INS_and, EA_4BYTE, targetReg, targetReg, 0x1);

    genProduceReg(simdNode);
}

//--------------------------------------------------------------------------------
// genSIMDIntrinsicDotProduct: Generate code for SIMD Intrinsic Dot Product.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicDotProduct(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicDotProduct);

    GenTree*  op1      = simdNode->gtGetOp1();
    GenTree*  op2      = simdNode->gtGetOp2();
    var_types baseType = simdNode->gtSIMDBaseType;
    var_types simdType = op1->TypeGet();

    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);

    var_types targetType = simdNode->TypeGet();
    assert(targetType == baseType);

    genConsumeOperands(simdNode);
    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;
    regNumber tmpReg = targetReg;

    if (!varTypeIsFloating(baseType))
    {
        tmpReg = simdNode->GetSingleTempReg(RBM_ALLFLOAT);
    }

    instruction ins  = getOpForSIMDIntrinsic(SIMDIntrinsicMul, baseType);
    emitAttr    attr = (simdNode->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = genGetSimdInsOpt(attr, baseType);

    // Vector multiply
    getEmitter()->emitIns_R_R_R(ins, attr, tmpReg, op1Reg, op2Reg, opt);

    if ((simdNode->gtFlags & GTF_SIMD12_OP) != 0)
    {
        // For 12Byte vectors we must zero upper bits to get correct dot product
        // We do not assume upper bits are zero.
        getEmitter()->emitIns_R_R_I(INS_ins, EA_4BYTE, tmpReg, REG_ZR, 3);
    }

    // Vector add horizontal
    if (varTypeIsFloating(baseType))
    {
        if (baseType == TYP_FLOAT)
        {
            if (opt == INS_OPTS_4S)
            {
                getEmitter()->emitIns_R_R_R(INS_faddp, attr, tmpReg, tmpReg, tmpReg, INS_OPTS_4S);
            }
            getEmitter()->emitIns_R_R(INS_faddp, EA_4BYTE, targetReg, tmpReg);
        }
        else
        {
            getEmitter()->emitIns_R_R(INS_faddp, EA_8BYTE, targetReg, tmpReg);
        }
    }
    else
    {
        ins = varTypeIsUnsigned(baseType) ? INS_uaddlv : INS_saddlv;

        getEmitter()->emitIns_R_R(ins, attr, tmpReg, tmpReg, opt);

        // Mov to integer register
        if (varTypeIsUnsigned(baseType) || (genTypeSize(baseType) < 4))
        {
            getEmitter()->emitIns_R_R_I(INS_mov, emitTypeSize(baseType), targetReg, tmpReg, 0);
        }
        else
        {
            getEmitter()->emitIns_R_R_I(INS_smov, emitActualTypeSize(baseType), targetReg, tmpReg, 0);
        }
    }

    genProduceReg(simdNode);
}

//------------------------------------------------------------------------------------
// genSIMDIntrinsicGetItem: Generate code for SIMD Intrinsic get element at index i.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicGetItem(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicGetItem);

    GenTree*  op1      = simdNode->gtGetOp1();
    GenTree*  op2      = simdNode->gtGetOp2();
    var_types simdType = op1->TypeGet();
    assert(varTypeIsSIMD(simdType));

    // op1 of TYP_SIMD12 should be considered as TYP_SIMD16
    if (simdType == TYP_SIMD12)
    {
        simdType = TYP_SIMD16;
    }

    var_types baseType  = simdNode->gtSIMDBaseType;
    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);
    var_types targetType = simdNode->TypeGet();
    assert(targetType == genActualType(baseType));

    // GetItem has 2 operands:
    // - the source of SIMD type (op1)
    // - the index of the value to be returned.
    genConsumeOperands(simdNode);

    emitAttr baseTypeSize  = emitTypeSize(baseType);
    unsigned baseTypeScale = genLog2(EA_SIZE_IN_BYTES(baseTypeSize));

    if (op2->IsCnsIntOrI())
    {
        assert(op2->isContained());

        ssize_t index = op2->gtIntCon.gtIconVal;

        // We only need to generate code for the get if the index is valid
        // If the index is invalid, previously generated for the range check will throw
        if (getEmitter()->isValidVectorIndex(emitTypeSize(simdType), baseTypeSize, index))
        {
            if (op1->isContained())
            {
                int         offset = (int)index * genTypeSize(baseType);
                instruction ins    = ins_Load(baseType);
                baseTypeSize       = varTypeIsFloating(baseType)
                                   ? baseTypeSize
                                   : getEmitter()->emitInsAdjustLoadStoreAttr(ins, baseTypeSize);

                assert(!op1->isUsedFromReg());

                if (op1->OperIsLocal())
                {
                    unsigned varNum = op1->gtLclVarCommon.gtLclNum;

                    getEmitter()->emitIns_R_S(ins, baseTypeSize, targetReg, varNum, offset);
                }
                else
                {
                    assert(op1->OperGet() == GT_IND);

                    GenTree* addr = op1->AsIndir()->Addr();
                    assert(!addr->isContained());
                    regNumber baseReg = addr->gtRegNum;

                    // ldr targetReg, [baseReg, #offset]
                    getEmitter()->emitIns_R_R_I(ins, baseTypeSize, targetReg, baseReg, offset);
                }
            }
            else
            {
                assert(op1->isUsedFromReg());
                regNumber srcReg = op1->gtRegNum;

                instruction ins;
                if (varTypeIsFloating(baseType))
                {
                    assert(genIsValidFloatReg(targetReg));
                    // dup targetReg, srcReg[#index]
                    ins = INS_dup;
                }
                else
                {
                    assert(genIsValidIntReg(targetReg));
                    if (varTypeIsUnsigned(baseType) || (baseTypeSize == EA_8BYTE))
                    {
                        // umov targetReg, srcReg[#index]
                        ins = INS_umov;
                    }
                    else
                    {
                        // smov targetReg, srcReg[#index]
                        ins = INS_smov;
                    }
                }
                getEmitter()->emitIns_R_R_I(ins, baseTypeSize, targetReg, srcReg, index);
            }
        }
    }
    else
    {
        assert(!op2->isContained());

        regNumber baseReg  = REG_NA;
        regNumber indexReg = op2->gtRegNum;

        if (op1->isContained())
        {
            // Optimize the case of op1 is in memory and trying to access ith element.
            assert(!op1->isUsedFromReg());
            if (op1->OperIsLocal())
            {
                unsigned varNum = op1->gtLclVarCommon.gtLclNum;

                baseReg = simdNode->ExtractTempReg();

                // Load the address of varNum
                getEmitter()->emitIns_R_S(INS_lea, EA_PTRSIZE, baseReg, varNum, 0);
            }
            else
            {
                // Require GT_IND addr to be not contained.
                assert(op1->OperGet() == GT_IND);

                GenTree* addr = op1->AsIndir()->Addr();
                assert(!addr->isContained());

                baseReg = addr->gtRegNum;
            }
        }
        else
        {
            assert(op1->isUsedFromReg());
            regNumber srcReg = op1->gtRegNum;

            unsigned simdInitTempVarNum = compiler->lvaSIMDInitTempVarNum;
            noway_assert(compiler->lvaSIMDInitTempVarNum != BAD_VAR_NUM);

            baseReg = simdNode->ExtractTempReg();

            // Load the address of simdInitTempVarNum
            getEmitter()->emitIns_R_S(INS_lea, EA_PTRSIZE, baseReg, simdInitTempVarNum, 0);

            // Store the vector to simdInitTempVarNum
            getEmitter()->emitIns_R_R(INS_str, emitTypeSize(simdType), srcReg, baseReg);
        }

        assert(genIsValidIntReg(indexReg));
        assert(genIsValidIntReg(baseReg));
        assert(baseReg != indexReg);

        // Load item at baseReg[index]
        getEmitter()->emitIns_R_R_R_Ext(ins_Load(baseType), baseTypeSize, targetReg, baseReg, indexReg, INS_OPTS_LSL,
                                        baseTypeScale);
    }

    genProduceReg(simdNode);
}

//------------------------------------------------------------------------------------
// genSIMDIntrinsicSetItem: Generate code for SIMD Intrinsic set element at index i.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
void CodeGen::genSIMDIntrinsicSetItem(GenTreeSIMD* simdNode)
{
    // Determine index based on intrinsic ID
    int index = -1;
    switch (simdNode->gtSIMDIntrinsicID)
    {
        case SIMDIntrinsicSetX:
            index = 0;
            break;
        case SIMDIntrinsicSetY:
            index = 1;
            break;
        case SIMDIntrinsicSetZ:
            index = 2;
            break;
        case SIMDIntrinsicSetW:
            index = 3;
            break;

        default:
            unreached();
    }
    assert(index != -1);

    // op1 is the SIMD vector
    // op2 is the value to be set
    GenTree* op1 = simdNode->gtGetOp1();
    GenTree* op2 = simdNode->gtGetOp2();

    var_types baseType  = simdNode->gtSIMDBaseType;
    regNumber targetReg = simdNode->gtRegNum;
    assert(targetReg != REG_NA);
    var_types targetType = simdNode->TypeGet();
    assert(varTypeIsSIMD(targetType));

    assert(op2->TypeGet() == baseType);
    assert(simdNode->gtSIMDSize >= ((index + 1) * genTypeSize(baseType)));

    genConsumeOperands(simdNode);
    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;

    assert(genIsValidFloatReg(targetReg));
    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidIntReg(op2Reg) || genIsValidFloatReg(op2Reg));
    assert(targetReg != op2Reg);

    emitAttr attr = emitTypeSize(baseType);

    // Insert mov if register assignment requires it
    getEmitter()->emitIns_R_R(INS_mov, EA_16BYTE, targetReg, op1Reg);

    if (genIsValidIntReg(op2Reg))
    {
        getEmitter()->emitIns_R_R_I(INS_ins, attr, targetReg, op2Reg, index);
    }
    else
    {
        getEmitter()->emitIns_R_R_I_I(INS_ins, attr, targetReg, op2Reg, index, 0);
    }

    genProduceReg(simdNode);
}

//-----------------------------------------------------------------------------
// genSIMDIntrinsicUpperSave: save the upper half of a TYP_SIMD16 vector to
//                            the given register, if any, or to memory.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
// Notes:
//    The upper half of all SIMD registers are volatile, even the callee-save registers.
//    When a 16-byte SIMD value is live across a call, the register allocator will use this intrinsic
//    to cause the upper half to be saved.  It will first attempt to find another, unused, callee-save
//    register.  If such a register cannot be found, it will save it to an available caller-save register.
//    In that case, this node will be marked GTF_SPILL, which will cause genProduceReg to save the 8 byte
//    value to the stack.  (Note that if there are no caller-save registers available, the entire 16 byte
//    value will be spilled to the stack.)
//
void CodeGen::genSIMDIntrinsicUpperSave(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicUpperSave);

    GenTree* op1 = simdNode->gtGetOp1();
    assert(op1->IsLocal());
    assert(emitTypeSize(op1->TypeGet()) == 16);
    regNumber targetReg = simdNode->gtRegNum;
    regNumber op1Reg    = genConsumeReg(op1);
    assert(op1Reg != REG_NA);
    assert(targetReg != REG_NA);
    getEmitter()->emitIns_R_R_I_I(INS_mov, EA_8BYTE, targetReg, op1Reg, 0, 1);

    genProduceReg(simdNode);
}

//-----------------------------------------------------------------------------
// genSIMDIntrinsicUpperRestore: Restore the upper half of a TYP_SIMD16 vector to
//                               the given register, if any, or to memory.
//
// Arguments:
//    simdNode - The GT_SIMD node
//
// Return Value:
//    None.
//
// Notes:
//    For consistency with genSIMDIntrinsicUpperSave, and to ensure that lclVar nodes always
//    have their home register, this node has its targetReg on the lclVar child, and its source
//    on the simdNode.
//    Regarding spill, please see the note above on genSIMDIntrinsicUpperSave.  If we have spilled
//    an upper-half to a caller save register, this node will be marked GTF_SPILLED.  However, unlike
//    most spill scenarios, the saved tree will be different from the restored tree, but the spill
//    restore logic, which is triggered by the call to genConsumeReg, requires us to provide the
//    spilled tree (saveNode) in order to perform the reload.  We can easily find that tree,
//    as it is in the spill descriptor for the register from which it was saved.
//
void CodeGen::genSIMDIntrinsicUpperRestore(GenTreeSIMD* simdNode)
{
    assert(simdNode->gtSIMDIntrinsicID == SIMDIntrinsicUpperRestore);

    GenTree* op1 = simdNode->gtGetOp1();
    assert(op1->IsLocal());
    assert(emitTypeSize(op1->TypeGet()) == 16);
    regNumber srcReg    = simdNode->gtRegNum;
    regNumber lclVarReg = genConsumeReg(op1);
    unsigned  varNum    = op1->AsLclVarCommon()->gtLclNum;
    assert(lclVarReg != REG_NA);
    assert(srcReg != REG_NA);
    if (simdNode->gtFlags & GTF_SPILLED)
    {
        GenTree* saveNode = regSet.rsSpillDesc[srcReg]->spillTree;
        noway_assert(saveNode != nullptr && (saveNode->gtRegNum == srcReg));
        genConsumeReg(saveNode);
    }
    getEmitter()->emitIns_R_R_I_I(INS_mov, EA_8BYTE, lclVarReg, srcReg, 1, 0);
}

//-----------------------------------------------------------------------------
// genStoreIndTypeSIMD12: store indirect a TYP_SIMD12 (i.e. Vector3) to memory.
// Since Vector3 is not a hardware supported write size, it is performed
// as two writes: 8 byte followed by 4-byte.
//
// Arguments:
//    treeNode - tree node that is attempting to store indirect
//
//
// Return Value:
//    None.
//
void CodeGen::genStoreIndTypeSIMD12(GenTree* treeNode)
{
    assert(treeNode->OperGet() == GT_STOREIND);

    GenTree* addr = treeNode->gtOp.gtOp1;
    GenTree* data = treeNode->gtOp.gtOp2;

    // addr and data should not be contained.
    assert(!data->isContained());
    assert(!addr->isContained());

#ifdef DEBUG
    // Should not require a write barrier
    GCInfo::WriteBarrierForm writeBarrierForm = gcInfo.gcIsWriteBarrierCandidate(treeNode, data);
    assert(writeBarrierForm == GCInfo::WBF_NoBarrier);
#endif

    genConsumeOperands(treeNode->AsOp());

    // Need an addtional integer register to extract upper 4 bytes from data.
    regNumber tmpReg = treeNode->GetSingleTempReg();
    assert(tmpReg != addr->gtRegNum);

    // 8-byte write
    getEmitter()->emitIns_R_R(ins_Store(TYP_DOUBLE), EA_8BYTE, data->gtRegNum, addr->gtRegNum);

    // Extract upper 4-bytes from data
    getEmitter()->emitIns_R_R_I(INS_mov, EA_4BYTE, tmpReg, data->gtRegNum, 2);

    // 4-byte write
    getEmitter()->emitIns_R_R_I(INS_str, EA_4BYTE, tmpReg, addr->gtRegNum, 8);
}

//-----------------------------------------------------------------------------
// genLoadIndTypeSIMD12: load indirect a TYP_SIMD12 (i.e. Vector3) value.
// Since Vector3 is not a hardware supported write size, it is performed
// as two loads: 8 byte followed by 4-byte.
//
// Arguments:
//    treeNode - tree node of GT_IND
//
//
// Return Value:
//    None.
//
void CodeGen::genLoadIndTypeSIMD12(GenTree* treeNode)
{
    assert(treeNode->OperGet() == GT_IND);

    GenTree*  addr      = treeNode->gtOp.gtOp1;
    regNumber targetReg = treeNode->gtRegNum;

    assert(!addr->isContained());

    regNumber operandReg = genConsumeReg(addr);

    // Need an addtional int register to read upper 4 bytes, which is different from targetReg
    regNumber tmpReg = treeNode->GetSingleTempReg();

    // 8-byte read
    getEmitter()->emitIns_R_R(ins_Load(TYP_DOUBLE), EA_8BYTE, targetReg, addr->gtRegNum);

    // 4-byte read
    getEmitter()->emitIns_R_R_I(INS_ldr, EA_4BYTE, tmpReg, addr->gtRegNum, 8);

    // Insert upper 4-bytes into data
    getEmitter()->emitIns_R_R_I(INS_mov, EA_4BYTE, targetReg, tmpReg, 2);

    genProduceReg(treeNode);
}

//-----------------------------------------------------------------------------
// genStoreLclTypeSIMD12: store a TYP_SIMD12 (i.e. Vector3) type field.
// Since Vector3 is not a hardware supported write size, it is performed
// as two stores: 8 byte followed by 4-byte.
//
// Arguments:
//    treeNode - tree node that is attempting to store TYP_SIMD12 field
//
// Return Value:
//    None.
//
void CodeGen::genStoreLclTypeSIMD12(GenTree* treeNode)
{
    assert((treeNode->OperGet() == GT_STORE_LCL_FLD) || (treeNode->OperGet() == GT_STORE_LCL_VAR));

    unsigned offs   = 0;
    unsigned varNum = treeNode->gtLclVarCommon.gtLclNum;
    assert(varNum < compiler->lvaCount);

    if (treeNode->OperGet() == GT_STORE_LCL_FLD)
    {
        offs = treeNode->gtLclFld.gtLclOffs;
    }

    GenTree* op1 = treeNode->gtOp.gtOp1;
    assert(!op1->isContained());
    regNumber operandReg = genConsumeReg(op1);

    // Need an addtional integer register to extract upper 4 bytes from data.
    regNumber tmpReg = treeNode->GetSingleTempReg();

    // store lower 8 bytes
    getEmitter()->emitIns_S_R(ins_Store(TYP_DOUBLE), EA_8BYTE, operandReg, varNum, offs);

    // Extract upper 4-bytes from data
    getEmitter()->emitIns_R_R_I(INS_mov, EA_4BYTE, tmpReg, operandReg, 2);

    // 4-byte write
    getEmitter()->emitIns_S_R(INS_str, EA_4BYTE, tmpReg, varNum, offs + 8);
}

#endif // FEATURE_SIMD

#ifdef FEATURE_HW_INTRINSICS
#include "hwintrinsic.h"

instruction CodeGen::getOpForHWIntrinsic(GenTreeHWIntrinsic* node, var_types instrType)
{
    NamedIntrinsic intrinsicID = node->gtHWIntrinsicId;

    unsigned int instrTypeIndex = varTypeIsFloating(instrType) ? 0 : varTypeIsUnsigned(instrType) ? 2 : 1;

    instruction ins = HWIntrinsicInfo::lookup(intrinsicID).instrs[instrTypeIndex];
    assert(ins != INS_invalid);

    return ins;
}

//------------------------------------------------------------------------
// genHWIntrinsic: Produce code for a GT_HWIntrinsic node.
//
// This is the main routine which in turn calls the genHWIntrinsicXXX() routines.
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsic(GenTreeHWIntrinsic* node)
{
    NamedIntrinsic intrinsicID = node->gtHWIntrinsicId;

    switch (HWIntrinsicInfo::lookup(intrinsicID).form)
    {
        case HWIntrinsicInfo::UnaryOp:
            genHWIntrinsicUnaryOp(node);
            break;
        case HWIntrinsicInfo::CrcOp:
            genHWIntrinsicCrcOp(node);
            break;
        case HWIntrinsicInfo::SimdBinaryOp:
            genHWIntrinsicSimdBinaryOp(node);
            break;
        case HWIntrinsicInfo::SimdExtractOp:
            genHWIntrinsicSimdExtractOp(node);
            break;
        case HWIntrinsicInfo::SimdInsertOp:
            genHWIntrinsicSimdInsertOp(node);
            break;
        case HWIntrinsicInfo::SimdSelectOp:
            genHWIntrinsicSimdSelectOp(node);
            break;
        case HWIntrinsicInfo::SimdSetAllOp:
            genHWIntrinsicSimdSetAllOp(node);
            break;
        case HWIntrinsicInfo::SimdUnaryOp:
            genHWIntrinsicSimdUnaryOp(node);
            break;
        case HWIntrinsicInfo::SimdBinaryRMWOp:
            genHWIntrinsicSimdBinaryRMWOp(node);
            break;
        case HWIntrinsicInfo::SimdTernaryRMWOp:
            genHWIntrinsicSimdTernaryRMWOp(node);
            break;
        case HWIntrinsicInfo::Sha1HashOp:
            genHWIntrinsicShaHashOp(node);
            break;
        case HWIntrinsicInfo::Sha1RotateOp:
            genHWIntrinsicShaRotateOp(node);
            break;

        default:
            NYI("HWIntrinsic form not implemented");
    }
}

//------------------------------------------------------------------------
// genHWIntrinsicUnaryOp:
//
// Produce code for a GT_HWIntrinsic node with form UnaryOp.
//
// Consumes one scalar operand produces a scalar
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicUnaryOp(GenTreeHWIntrinsic* node)
{
    GenTree*  op1       = node->gtGetOp1();
    regNumber targetReg = node->gtRegNum;
    emitAttr  attr      = emitActualTypeSize(op1->TypeGet());

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeOperands(node);

    regNumber op1Reg = op1->gtRegNum;

    instruction ins = getOpForHWIntrinsic(node, node->TypeGet());

    getEmitter()->emitIns_R_R(ins, attr, targetReg, op1Reg);

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicCrcOp:
//
// Produce code for a GT_HWIntrinsic node with form CrcOp.
//
// Consumes two scalar operands and produces a scalar result
//
// This form differs from BinaryOp because the attr depends on the size of op2
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicCrcOp(GenTreeHWIntrinsic* node)
{
    NYI("genHWIntrinsicCrcOp not implemented");
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdBinaryOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdBinaryOp.
//
// Consumes two SIMD operands and produces a SIMD result
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdBinaryOp(GenTreeHWIntrinsic* node)
{
    GenTree*  op1       = node->gtGetOp1();
    GenTree*  op2       = node->gtGetOp2();
    var_types baseType  = node->gtSIMDBaseType;
    regNumber targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeOperands(node);

    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(op2Reg));
    assert(genIsValidFloatReg(targetReg));

    instruction ins  = getOpForHWIntrinsic(node, baseType);
    emitAttr    attr = (node->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = genGetSimdInsOpt(attr, baseType);

    getEmitter()->emitIns_R_R_R(ins, attr, targetReg, op1Reg, op2Reg, opt);

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicSwitchTable: generate the jump-table for imm-intrinsics
//    with non-constant argument
//
// Arguments:
//    swReg      - register containing the switch case to execute
//    tmpReg     - temporary integer register for calculating the switch indirect branch target
//    swMax      - the number of switch cases.
//    emitSwCase - lambda to generate an individual switch case
//
// Notes:
//    Used for cases where an instruction only supports immediate operands,
//    but at jit time the operand is not a constant.
//
//    The importer is responsible for inserting an upstream range check
//    (GT_HW_INTRINSIC_CHK) for swReg, so no range check is needed here.
//
template <typename HWIntrinsicSwitchCaseBody>
void CodeGen::genHWIntrinsicSwitchTable(regNumber                 swReg,
                                        regNumber                 tmpReg,
                                        int                       swMax,
                                        HWIntrinsicSwitchCaseBody emitSwCase)
{
    assert(swMax > 0);
    assert(swMax <= 256);

    assert(genIsValidIntReg(tmpReg));
    assert(genIsValidIntReg(swReg));

    BasicBlock* switchTableBeg = genCreateTempLabel();
    BasicBlock* switchTableEnd = genCreateTempLabel();

    // Calculate switch target
    //
    // Each switch table case needs exactly 8 bytes of code.
    switchTableBeg->bbFlags |= BBF_JMP_TARGET;

    // tmpReg = switchTableBeg
    getEmitter()->emitIns_R_L(INS_adr, EA_PTRSIZE, switchTableBeg, tmpReg);

    // tmpReg = switchTableBeg + swReg * 8
    getEmitter()->emitIns_R_R_R_I(INS_add, EA_PTRSIZE, tmpReg, tmpReg, swReg, 3, INS_OPTS_LSL);

    // br tmpReg
    getEmitter()->emitIns_R(INS_br, EA_PTRSIZE, tmpReg);

    genDefineTempLabel(switchTableBeg);
    for (int i = 0; i < swMax; ++i)
    {
        unsigned prevInsCount = getEmitter()->emitInsCount;

        emitSwCase(i);

        assert(getEmitter()->emitInsCount == prevInsCount + 1);

        inst_JMP(EJ_jmp, switchTableEnd);

        assert(getEmitter()->emitInsCount == prevInsCount + 2);
    }
    genDefineTempLabel(switchTableEnd);
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdExtractOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdExtractOp.
//
// Consumes one SIMD operand and one scalar
//
// The element index operand is typically a const immediate
// When it is not, a switch table is generated
//
// See genHWIntrinsicSwitchTable comments
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdExtractOp(GenTreeHWIntrinsic* node)
{
    GenTree*  op1        = node->gtGetOp1();
    GenTree*  op2        = node->gtGetOp2();
    var_types simdType   = op1->TypeGet();
    var_types targetType = node->TypeGet();
    regNumber targetReg  = node->gtRegNum;

    assert(targetReg != REG_NA);

    genConsumeOperands(node);

    regNumber op1Reg = op1->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));

    emitAttr baseTypeSize = emitTypeSize(targetType);

    int elements = emitTypeSize(simdType) / baseTypeSize;

    auto emitSwCase = [&](int element) {
        assert(element >= 0);
        assert(element < elements);

        if (varTypeIsFloating(targetType))
        {
            assert(genIsValidFloatReg(targetReg));
            getEmitter()->emitIns_R_R_I_I(INS_mov, baseTypeSize, targetReg, op1Reg, 0, element);
        }
        else if (varTypeIsUnsigned(targetType) || (baseTypeSize == EA_8BYTE))
        {
            assert(genIsValidIntReg(targetReg));
            getEmitter()->emitIns_R_R_I(INS_umov, baseTypeSize, targetReg, op1Reg, element);
        }
        else
        {
            assert(genIsValidIntReg(targetReg));
            getEmitter()->emitIns_R_R_I(INS_smov, baseTypeSize, targetReg, op1Reg, element);
        }
    };

    if (op2->isContainedIntOrIImmed())
    {
        int element = (int)op2->AsIntConCommon()->IconValue();

        emitSwCase(element);
    }
    else
    {
        regNumber elementReg = op2->gtRegNum;
        regNumber tmpReg     = node->GetSingleTempReg();

        genHWIntrinsicSwitchTable(elementReg, tmpReg, elements, emitSwCase);
    }

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdInsertOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdInsertOp.
//
// Consumes one SIMD operand and two scalars
//
// The element index operand is typically a const immediate
// When it is not, a switch table is generated
//
// See genHWIntrinsicSwitchTable comments
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdInsertOp(GenTreeHWIntrinsic* node)
{
    GenTreeArgList* argList   = node->gtGetOp1()->AsArgList();
    GenTree*        op1       = argList->Current();
    GenTree*        op2       = argList->Rest()->Current();
    GenTree*        op3       = argList->Rest()->Rest()->Current();
    var_types       simdType  = op1->TypeGet();
    var_types       baseType  = node->gtSIMDBaseType;
    regNumber       targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);

    genConsumeRegs(op1);
    genConsumeRegs(op2);
    genConsumeRegs(op3);

    regNumber op1Reg = op1->gtRegNum;

    assert(genIsValidFloatReg(targetReg));
    assert(genIsValidFloatReg(op1Reg));

    emitAttr baseTypeSize = emitTypeSize(baseType);

    int elements = emitTypeSize(simdType) / baseTypeSize;

    if (targetReg != op1Reg)
    {
        getEmitter()->emitIns_R_R(INS_mov, baseTypeSize, targetReg, op1Reg);
    }

    if (op3->isContained())
    {
        // Handle vector element to vector element case
        //
        // If op3 is contained this is because lowering found an opportunity to contain a Simd.Extract in a Simd.Insert
        //
        regNumber op3Reg = op3->gtGetOp1()->gtRegNum;

        assert(genIsValidFloatReg(op3Reg));

        // op3 containment currently only occurs when
        //   + op3 is a Simd.Extract() (gtHWIntrinsicId == NI_ARM64_SIMD_GetItem)
        //   + element & srcLane are immediate constants
        assert(op2->isContainedIntOrIImmed());
        assert(op3->OperIs(GT_HWIntrinsic));
        assert(op3->AsHWIntrinsic()->gtHWIntrinsicId == NI_ARM64_SIMD_GetItem);
        assert(op3->gtGetOp2()->isContainedIntOrIImmed());

        int element = (int)op2->AsIntConCommon()->IconValue();
        int srcLane = (int)op3->gtGetOp2()->AsIntConCommon()->IconValue();

        // Emit mov targetReg[element], op3Reg[srcLane]
        getEmitter()->emitIns_R_R_I_I(INS_mov, baseTypeSize, targetReg, op3Reg, element, srcLane);
    }
    else
    {
        // Handle scalar to vector element case
        // TODO-ARM64-CQ handle containing op3 scalar const where possible
        regNumber op3Reg = op3->gtRegNum;

        auto emitSwCase = [&](int element) {
            assert(element >= 0);
            assert(element < elements);

            if (varTypeIsFloating(baseType))
            {
                assert(genIsValidFloatReg(op3Reg));
                getEmitter()->emitIns_R_R_I_I(INS_mov, baseTypeSize, targetReg, op3Reg, element, 0);
            }
            else
            {
                assert(genIsValidIntReg(op3Reg));
                getEmitter()->emitIns_R_R_I(INS_mov, baseTypeSize, targetReg, op3Reg, element);
            }
        };

        if (op2->isContainedIntOrIImmed())
        {
            int element = (int)op2->AsIntConCommon()->IconValue();

            emitSwCase(element);
        }
        else
        {
            regNumber elementReg = op2->gtRegNum;
            regNumber tmpReg     = node->GetSingleTempReg();

            genHWIntrinsicSwitchTable(elementReg, tmpReg, elements, emitSwCase);
        }
    }

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdSelectOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdSelectOp.
//
// Consumes three SIMD operands and produces a SIMD result
//
// This intrinsic form requires one of the source registers to be the
// destination register.  Inserts a INS_mov if this requirement is not met.
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdSelectOp(GenTreeHWIntrinsic* node)
{
    GenTreeArgList* argList   = node->gtGetOp1()->AsArgList();
    GenTree*        op1       = argList->Current();
    GenTree*        op2       = argList->Rest()->Current();
    GenTree*        op3       = argList->Rest()->Rest()->Current();
    var_types       baseType  = node->gtSIMDBaseType;
    regNumber       targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeRegs(op1);
    genConsumeRegs(op2);
    genConsumeRegs(op3);

    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;
    regNumber op3Reg = op3->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(op2Reg));
    assert(genIsValidFloatReg(op3Reg));
    assert(genIsValidFloatReg(targetReg));

    emitAttr attr = (node->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;

    // Arm64 has three bit select forms; each uses three source registers
    // One of the sources is also the destination
    if (targetReg == op3Reg)
    {
        // op3 is target use bit insert if true
        // op3 = op3 ^ (op1 & (op2 ^ op3))
        getEmitter()->emitIns_R_R_R(INS_bit, attr, op3Reg, op2Reg, op1Reg);
    }
    else if (targetReg == op2Reg)
    {
        // op2 is target use bit insert if false
        // op2 = op2 ^ (~op1 & (op2 ^ op3))
        getEmitter()->emitIns_R_R_R(INS_bif, attr, op2Reg, op3Reg, op1Reg);
    }
    else
    {
        if (targetReg != op1Reg)
        {
            // target is not one of the sources, copy op1 to use bit select form
            getEmitter()->emitIns_R_R(INS_mov, attr, targetReg, op1Reg);
        }
        // use bit select
        // targetReg = op3 ^ (targetReg & (op2 ^ op3))
        getEmitter()->emitIns_R_R_R(INS_bsl, attr, targetReg, op2Reg, op3Reg);
    }

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdSetAllOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdSetAllOp.
//
// Consumes single scalar operand and produces a SIMD result
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdSetAllOp(GenTreeHWIntrinsic* node)
{
    GenTree*  op1       = node->gtGetOp1();
    var_types baseType  = node->gtSIMDBaseType;
    regNumber targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeOperands(node);

    regNumber op1Reg = op1->gtRegNum;

    assert(genIsValidFloatReg(targetReg));
    assert(genIsValidIntReg(op1Reg) || genIsValidFloatReg(op1Reg));

    instruction ins  = getOpForHWIntrinsic(node, baseType);
    emitAttr    attr = (node->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = genGetSimdInsOpt(attr, baseType);

    // TODO-ARM64-CQ Support contained immediate cases

    if (genIsValidIntReg(op1Reg))
    {
        getEmitter()->emitIns_R_R(ins, attr, targetReg, op1Reg, opt);
    }
    else
    {
        getEmitter()->emitIns_R_R_I(ins, attr, targetReg, op1Reg, 0, opt);
    }

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdUnaryOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdUnaryOp.
//
// Consumes single SIMD operand and produces a SIMD result
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdUnaryOp(GenTreeHWIntrinsic* node)
{
    GenTree*  op1       = node->gtGetOp1();
    var_types baseType  = node->gtSIMDBaseType;
    regNumber targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeOperands(node);

    regNumber op1Reg = op1->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(targetReg));

    instruction ins  = getOpForHWIntrinsic(node, baseType);
    emitAttr    attr = (node->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = genGetSimdInsOpt(attr, baseType);

    getEmitter()->emitIns_R_R(ins, attr, targetReg, op1Reg, opt);

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdBinaryRMWOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdBinaryRMWOp.
//
// Consumes two SIMD operands and produces a SIMD result.
// First operand is both source and destination.
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdBinaryRMWOp(GenTreeHWIntrinsic* node)
{
    GenTree*  op1       = node->gtGetOp1();
    GenTree*  op2       = node->gtGetOp2();
    var_types baseType  = node->gtSIMDBaseType;
    regNumber targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);

    genConsumeOperands(node);

    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(op2Reg));
    assert(genIsValidFloatReg(targetReg));

    instruction ins  = getOpForHWIntrinsic(node, baseType);
    emitAttr    attr = (node->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;
    insOpts     opt  = genGetSimdInsOpt(attr, baseType);

    if (targetReg != op1Reg)
    {
        getEmitter()->emitIns_R_R(INS_mov, attr, targetReg, op1Reg);
    }
    getEmitter()->emitIns_R_R(ins, attr, targetReg, op2Reg, opt);

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicSimdTernaryRMWOp:
//
// Produce code for a GT_HWIntrinsic node with form SimdTernaryRMWOp
//
// Consumes three SIMD operands and produces a SIMD result.
// First operand is both source and destination.
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicSimdTernaryRMWOp(GenTreeHWIntrinsic* node)
{
    GenTreeArgList* argList   = node->gtGetOp1()->AsArgList();
    GenTree*        op1       = argList->Current();
    GenTree*        op2       = argList->Rest()->Current();
    GenTree*        op3       = argList->Rest()->Rest()->Current();
    var_types       baseType  = node->gtSIMDBaseType;
    regNumber       targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeRegs(op1);
    genConsumeRegs(op2);
    genConsumeRegs(op3);

    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;
    regNumber op3Reg = op3->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(op2Reg));
    assert(genIsValidFloatReg(op3Reg));
    assert(genIsValidFloatReg(targetReg));
    assert(targetReg != op2Reg);
    assert(targetReg != op3Reg);

    instruction ins  = getOpForHWIntrinsic(node, baseType);
    emitAttr    attr = (node->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;

    if (targetReg != op1Reg)
    {
        getEmitter()->emitIns_R_R(INS_mov, attr, targetReg, op1Reg);
    }

    getEmitter()->emitIns_R_R_R(ins, attr, targetReg, op2Reg, op3Reg);

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicShaHashOp:
//
// Produce code for a GT_HWIntrinsic node with form Sha1HashOp.
// Used in Arm64 SHA1 Hash operations.
//
// Consumes three operands and returns a Simd result.
// First Simd operand is both source and destination.
// Second Operand is an unsigned int.
// Third operand is a simd operand.

// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicShaHashOp(GenTreeHWIntrinsic* node)
{
    GenTreeArgList* argList   = node->gtGetOp1()->AsArgList();
    GenTree*        op1       = argList->Current();
    GenTree*        op2       = argList->Rest()->Current();
    GenTree*        op3       = argList->Rest()->Rest()->Current();
    var_types       baseType  = node->gtSIMDBaseType;
    regNumber       targetReg = node->gtRegNum;

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeRegs(op1);
    genConsumeRegs(op2);
    genConsumeRegs(op3);

    regNumber op1Reg = op1->gtRegNum;
    regNumber op2Reg = op2->gtRegNum;
    regNumber op3Reg = op3->gtRegNum;

    assert(genIsValidFloatReg(op1Reg));
    assert(genIsValidFloatReg(op3Reg));
    assert(targetReg != op2Reg);
    assert(targetReg != op3Reg);

    instruction ins  = getOpForHWIntrinsic(node, baseType);
    emitAttr    attr = (node->gtSIMDSize > 8) ? EA_16BYTE : EA_8BYTE;

    assert(genIsValidIntReg(op2Reg));
    regNumber elementReg = op2->gtRegNum;
    regNumber tmpReg     = node->GetSingleTempReg(RBM_ALLFLOAT);

    getEmitter()->emitIns_R_R(INS_fmov, EA_4BYTE, tmpReg, elementReg);

    if (targetReg != op1Reg)
    {
        getEmitter()->emitIns_R_R(INS_mov, attr, targetReg, op1Reg);
    }

    getEmitter()->emitIns_R_R_R(ins, attr, targetReg, tmpReg, op3Reg);

    genProduceReg(node);
}

//------------------------------------------------------------------------
// genHWIntrinsicShaRotateOp:
//
// Produce code for a GT_HWIntrinsic node with form Sha1RotateOp.
// Used in Arm64 SHA1 Rotate operations.
//
// Consumes one integer operand and returns unsigned int result.
//
// Arguments:
//    node - the GT_HWIntrinsic node
//
// Return Value:
//    None.
//
void CodeGen::genHWIntrinsicShaRotateOp(GenTreeHWIntrinsic* node)
{
    GenTree*  op1       = node->gtGetOp1();
    regNumber targetReg = node->gtRegNum;
    emitAttr  attr      = emitActualTypeSize(node);

    assert(targetReg != REG_NA);
    var_types targetType = node->TypeGet();

    genConsumeOperands(node);

    instruction ins        = getOpForHWIntrinsic(node, node->TypeGet());
    regNumber   elementReg = op1->gtRegNum;
    regNumber   tmpReg     = node->GetSingleTempReg(RBM_ALLFLOAT);

    getEmitter()->emitIns_R_R(INS_fmov, EA_4BYTE, tmpReg, elementReg);
    getEmitter()->emitIns_R_R(ins, EA_4BYTE, tmpReg, tmpReg);
    getEmitter()->emitIns_R_R(INS_fmov, attr, targetReg, tmpReg);

    genProduceReg(node);
}

#endif // FEATURE_HW_INTRINSICS

/*****************************************************************************
 * Unit testing of the ARM64 emitter: generate a bunch of instructions into the prolog
 * (it's as good a place as any), then use COMPlus_JitLateDisasm=* to see if the late
 * disassembler thinks the instructions as the same as we do.
 */

// Uncomment "#define ALL_ARM64_EMITTER_UNIT_TESTS" to run all the unit tests here.
// After adding a unit test, and verifying it works, put it under this #ifdef, so we don't see it run every time.
//#define ALL_ARM64_EMITTER_UNIT_TESTS

#if defined(DEBUG)
void CodeGen::genArm64EmitterUnitTests()
{
    if (!verbose)
    {
        return;
    }

    if (!compiler->opts.altJit)
    {
        // No point doing this in a "real" JIT.
        return;
    }

    // Mark the "fake" instructions in the output.
    printf("*************** In genArm64EmitterUnitTests()\n");

    emitter* theEmitter = getEmitter();

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    // We use this:
    //      genDefineTempLabel(genCreateTempLabel());
    // to create artificial labels to help separate groups of tests.

    //
    // Loads/Stores basic general register
    //

    genDefineTempLabel(genCreateTempLabel());

    // ldr/str Xt, [reg]
    theEmitter->emitIns_R_R(INS_ldr, EA_8BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_ldrb, EA_1BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_ldrh, EA_2BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_str, EA_8BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_strb, EA_1BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_strh, EA_2BYTE, REG_R8, REG_R9);

    // ldr/str Wt, [reg]
    theEmitter->emitIns_R_R(INS_ldr, EA_4BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_ldrb, EA_1BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_ldrh, EA_2BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_str, EA_4BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_strb, EA_1BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_strh, EA_2BYTE, REG_R8, REG_R9);

    theEmitter->emitIns_R_R(INS_ldrsb, EA_4BYTE, REG_R8, REG_R9); // target Wt
    theEmitter->emitIns_R_R(INS_ldrsh, EA_4BYTE, REG_R8, REG_R9); // target Wt
    theEmitter->emitIns_R_R(INS_ldrsb, EA_8BYTE, REG_R8, REG_R9); // target Xt
    theEmitter->emitIns_R_R(INS_ldrsh, EA_8BYTE, REG_R8, REG_R9); // target Xt
    theEmitter->emitIns_R_R(INS_ldrsw, EA_8BYTE, REG_R8, REG_R9); // target Xt

    theEmitter->emitIns_R_R_I(INS_ldurb, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldurh, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_sturb, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_sturh, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldursb, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldursb, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldursh, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldursh, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldur, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldur, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_stur, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_stur, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldursw, EA_8BYTE, REG_R8, REG_R9, 1);

    // SP and ZR tests
    theEmitter->emitIns_R_R_I(INS_ldur, EA_8BYTE, REG_R8, REG_SP, 1);
    theEmitter->emitIns_R_R_I(INS_ldurb, EA_8BYTE, REG_ZR, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldurh, EA_8BYTE, REG_ZR, REG_SP, 1);

    // scaled
    theEmitter->emitIns_R_R_I(INS_ldrb, EA_1BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldrh, EA_2BYTE, REG_R8, REG_R9, 2);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_R8, REG_R9, 4);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_8BYTE, REG_R8, REG_R9, 8);

    // pre-/post-indexed (unscaled)
    theEmitter->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_R8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_R8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_8BYTE, REG_R8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_8BYTE, REG_R8, REG_R9, 1, INS_OPTS_PRE_INDEX);

    // ldar/stlr Rt, [reg]
    theEmitter->emitIns_R_R(INS_ldar, EA_8BYTE, REG_R9, REG_R8);
    theEmitter->emitIns_R_R(INS_ldar, EA_4BYTE, REG_R7, REG_R10);
    theEmitter->emitIns_R_R(INS_ldarb, EA_4BYTE, REG_R5, REG_R11);
    theEmitter->emitIns_R_R(INS_ldarh, EA_4BYTE, REG_R5, REG_R12);

    theEmitter->emitIns_R_R(INS_stlr, EA_8BYTE, REG_R9, REG_R8);
    theEmitter->emitIns_R_R(INS_stlr, EA_4BYTE, REG_R7, REG_R13);
    theEmitter->emitIns_R_R(INS_stlrb, EA_4BYTE, REG_R5, REG_R14);
    theEmitter->emitIns_R_R(INS_stlrh, EA_4BYTE, REG_R3, REG_R15);

    // ldaxr Rt, [reg]
    theEmitter->emitIns_R_R(INS_ldaxr, EA_8BYTE, REG_R9, REG_R8);
    theEmitter->emitIns_R_R(INS_ldaxr, EA_4BYTE, REG_R7, REG_R10);
    theEmitter->emitIns_R_R(INS_ldaxrb, EA_4BYTE, REG_R5, REG_R11);
    theEmitter->emitIns_R_R(INS_ldaxrh, EA_4BYTE, REG_R5, REG_R12);

    // ldxr Rt, [reg]
    theEmitter->emitIns_R_R(INS_ldxr, EA_8BYTE, REG_R9, REG_R8);
    theEmitter->emitIns_R_R(INS_ldxr, EA_4BYTE, REG_R7, REG_R10);
    theEmitter->emitIns_R_R(INS_ldxrb, EA_4BYTE, REG_R5, REG_R11);
    theEmitter->emitIns_R_R(INS_ldxrh, EA_4BYTE, REG_R5, REG_R12);

    // stxr Ws, Rt, [reg]
    theEmitter->emitIns_R_R_R(INS_stxr, EA_8BYTE, REG_R1, REG_R9, REG_R8);
    theEmitter->emitIns_R_R_R(INS_stxr, EA_4BYTE, REG_R3, REG_R7, REG_R13);
    theEmitter->emitIns_R_R_R(INS_stxrb, EA_4BYTE, REG_R8, REG_R5, REG_R14);
    theEmitter->emitIns_R_R_R(INS_stxrh, EA_4BYTE, REG_R12, REG_R3, REG_R15);

    // stlxr Ws, Rt, [reg]
    theEmitter->emitIns_R_R_R(INS_stlxr, EA_8BYTE, REG_R1, REG_R9, REG_R8);
    theEmitter->emitIns_R_R_R(INS_stlxr, EA_4BYTE, REG_R3, REG_R7, REG_R13);
    theEmitter->emitIns_R_R_R(INS_stlxrb, EA_4BYTE, REG_R8, REG_R5, REG_R14);
    theEmitter->emitIns_R_R_R(INS_stlxrh, EA_4BYTE, REG_R12, REG_R3, REG_R15);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // Compares
    //

    genDefineTempLabel(genCreateTempLabel());

    // cmp reg, reg
    theEmitter->emitIns_R_R(INS_cmp, EA_8BYTE, REG_R8, REG_R9);
    theEmitter->emitIns_R_R(INS_cmn, EA_8BYTE, REG_R8, REG_R9);

    // cmp reg, imm
    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, 0);
    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, 4095);
    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, 1 << 12);
    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, 4095 << 12);

    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, 0);
    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, 4095);
    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, 1 << 12);
    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, 4095 << 12);

    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, -1);
    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, -0xfff);
    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_I(INS_cmp, EA_8BYTE, REG_R8, 0xffffffffff800000LL);

    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, -1);
    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, -0xfff);
    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_I(INS_cmn, EA_8BYTE, REG_R8, 0xffffffffff800000LL);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    // R_R
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R(INS_cls, EA_8BYTE, REG_R1, REG_R12);
    theEmitter->emitIns_R_R(INS_clz, EA_8BYTE, REG_R2, REG_R13);
    theEmitter->emitIns_R_R(INS_rbit, EA_8BYTE, REG_R3, REG_R14);
    theEmitter->emitIns_R_R(INS_rev, EA_8BYTE, REG_R4, REG_R15);
    theEmitter->emitIns_R_R(INS_rev16, EA_8BYTE, REG_R5, REG_R0);
    theEmitter->emitIns_R_R(INS_rev32, EA_8BYTE, REG_R6, REG_R1);

    theEmitter->emitIns_R_R(INS_cls, EA_4BYTE, REG_R7, REG_R2);
    theEmitter->emitIns_R_R(INS_clz, EA_4BYTE, REG_R8, REG_R3);
    theEmitter->emitIns_R_R(INS_rbit, EA_4BYTE, REG_R9, REG_R4);
    theEmitter->emitIns_R_R(INS_rev, EA_4BYTE, REG_R10, REG_R5);
    theEmitter->emitIns_R_R(INS_rev16, EA_4BYTE, REG_R11, REG_R6);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_I
    //

    genDefineTempLabel(genCreateTempLabel());

    // mov reg, imm(i16,hw)
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x0000000000001234);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x0000000043210000);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x0000567800000000);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x8765000000000000);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0xFFFFFFFFFFFF1234);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0xFFFFFFFF4321FFFF);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0xFFFF5678FFFFFFFF);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x8765FFFFFFFFFFFF);

    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x00001234);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x87650000);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0xFFFF1234);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x4567FFFF);

    // mov reg, imm(N,r,s)
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x00FFFFF000000000);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x6666666666666666);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_SP, 0x7FFF00007FFF0000);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x5555555555555555);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0xE003E003E003E003);
    theEmitter->emitIns_R_I(INS_mov, EA_8BYTE, REG_R8, 0x0707070707070707);

    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x00FFFFF0);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x66666666);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x03FFC000);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x55555555);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0xE003E003);
    theEmitter->emitIns_R_I(INS_mov, EA_4BYTE, REG_R8, 0x07070707);

    theEmitter->emitIns_R_I(INS_tst, EA_8BYTE, REG_R8, 0xE003E003E003E003);
    theEmitter->emitIns_R_I(INS_tst, EA_8BYTE, REG_R8, 0x00FFFFF000000000);
    theEmitter->emitIns_R_I(INS_tst, EA_8BYTE, REG_R8, 0x6666666666666666);
    theEmitter->emitIns_R_I(INS_tst, EA_8BYTE, REG_R8, 0x0707070707070707);
    theEmitter->emitIns_R_I(INS_tst, EA_8BYTE, REG_R8, 0x7FFF00007FFF0000);
    theEmitter->emitIns_R_I(INS_tst, EA_8BYTE, REG_R8, 0x5555555555555555);

    theEmitter->emitIns_R_I(INS_tst, EA_4BYTE, REG_R8, 0xE003E003);
    theEmitter->emitIns_R_I(INS_tst, EA_4BYTE, REG_R8, 0x00FFFFF0);
    theEmitter->emitIns_R_I(INS_tst, EA_4BYTE, REG_R8, 0x66666666);
    theEmitter->emitIns_R_I(INS_tst, EA_4BYTE, REG_R8, 0x07070707);
    theEmitter->emitIns_R_I(INS_tst, EA_4BYTE, REG_R8, 0xFFF00000);
    theEmitter->emitIns_R_I(INS_tst, EA_4BYTE, REG_R8, 0x55555555);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R
    //

    genDefineTempLabel(genCreateTempLabel());

    // tst reg, reg
    theEmitter->emitIns_R_R(INS_tst, EA_8BYTE, REG_R7, REG_R10);

    // mov reg, reg
    theEmitter->emitIns_R_R(INS_mov, EA_8BYTE, REG_R7, REG_R10);
    theEmitter->emitIns_R_R(INS_mov, EA_8BYTE, REG_R8, REG_SP);
    theEmitter->emitIns_R_R(INS_mov, EA_8BYTE, REG_SP, REG_R9);

    theEmitter->emitIns_R_R(INS_mvn, EA_8BYTE, REG_R5, REG_R11);
    theEmitter->emitIns_R_R(INS_neg, EA_8BYTE, REG_R4, REG_R12);
    theEmitter->emitIns_R_R(INS_negs, EA_8BYTE, REG_R3, REG_R13);

    theEmitter->emitIns_R_R(INS_mov, EA_4BYTE, REG_R7, REG_R10);
    theEmitter->emitIns_R_R(INS_mvn, EA_4BYTE, REG_R5, REG_R11);
    theEmitter->emitIns_R_R(INS_neg, EA_4BYTE, REG_R4, REG_R12);
    theEmitter->emitIns_R_R(INS_negs, EA_4BYTE, REG_R3, REG_R13);

    theEmitter->emitIns_R_R(INS_sxtb, EA_8BYTE, REG_R7, REG_R10);
    theEmitter->emitIns_R_R(INS_sxth, EA_8BYTE, REG_R5, REG_R11);
    theEmitter->emitIns_R_R(INS_sxtw, EA_8BYTE, REG_R4, REG_R12);
    theEmitter->emitIns_R_R(INS_uxtb, EA_8BYTE, REG_R3, REG_R13); // map to Wt
    theEmitter->emitIns_R_R(INS_uxth, EA_8BYTE, REG_R2, REG_R14); // map to Wt

    theEmitter->emitIns_R_R(INS_sxtb, EA_4BYTE, REG_R7, REG_R10);
    theEmitter->emitIns_R_R(INS_sxth, EA_4BYTE, REG_R5, REG_R11);
    theEmitter->emitIns_R_R(INS_uxtb, EA_4BYTE, REG_R3, REG_R13);
    theEmitter->emitIns_R_R(INS_uxth, EA_4BYTE, REG_R2, REG_R14);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_I_I
    //

    genDefineTempLabel(genCreateTempLabel());

    // mov reg, imm(i16,hw)
    theEmitter->emitIns_R_I_I(INS_mov, EA_8BYTE, REG_R8, 0x1234, 0, INS_OPTS_LSL);
    theEmitter->emitIns_R_I_I(INS_mov, EA_8BYTE, REG_R8, 0x4321, 16, INS_OPTS_LSL);

    theEmitter->emitIns_R_I_I(INS_movk, EA_8BYTE, REG_R8, 0x4321, 16, INS_OPTS_LSL);
    theEmitter->emitIns_R_I_I(INS_movn, EA_8BYTE, REG_R8, 0x5678, 32, INS_OPTS_LSL);
    theEmitter->emitIns_R_I_I(INS_movz, EA_8BYTE, REG_R8, 0x8765, 48, INS_OPTS_LSL);

    theEmitter->emitIns_R_I_I(INS_movk, EA_4BYTE, REG_R8, 0x4321, 16, INS_OPTS_LSL);
    theEmitter->emitIns_R_I_I(INS_movn, EA_4BYTE, REG_R8, 0x5678, 16, INS_OPTS_LSL);
    theEmitter->emitIns_R_I_I(INS_movz, EA_4BYTE, REG_R8, 0x8765, 16, INS_OPTS_LSL);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_I
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_I(INS_lsl, EA_8BYTE, REG_R0, REG_R0, 1);
    theEmitter->emitIns_R_R_I(INS_lsl, EA_4BYTE, REG_R9, REG_R3, 18);
    theEmitter->emitIns_R_R_I(INS_lsr, EA_8BYTE, REG_R7, REG_R0, 37);
    theEmitter->emitIns_R_R_I(INS_lsr, EA_4BYTE, REG_R0, REG_R1, 2);
    theEmitter->emitIns_R_R_I(INS_asr, EA_8BYTE, REG_R2, REG_R3, 53);
    theEmitter->emitIns_R_R_I(INS_asr, EA_4BYTE, REG_R9, REG_R3, 18);

    theEmitter->emitIns_R_R_I(INS_and, EA_8BYTE, REG_R2, REG_R3, 0x5555555555555555);
    theEmitter->emitIns_R_R_I(INS_ands, EA_8BYTE, REG_R1, REG_R5, 0x6666666666666666);
    theEmitter->emitIns_R_R_I(INS_eor, EA_8BYTE, REG_R8, REG_R9, 0x0707070707070707);
    theEmitter->emitIns_R_R_I(INS_orr, EA_8BYTE, REG_SP, REG_R3, 0xFFFC000000000000);
    theEmitter->emitIns_R_R_I(INS_ands, EA_4BYTE, REG_R8, REG_R9, 0xE003E003);

    theEmitter->emitIns_R_R_I(INS_ror, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ror, EA_8BYTE, REG_R8, REG_R9, 31);
    theEmitter->emitIns_R_R_I(INS_ror, EA_8BYTE, REG_R8, REG_R9, 32);
    theEmitter->emitIns_R_R_I(INS_ror, EA_8BYTE, REG_R8, REG_R9, 63);

    theEmitter->emitIns_R_R_I(INS_ror, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ror, EA_4BYTE, REG_R8, REG_R9, 31);

    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_add, EA_4BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_sub, EA_8BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_adds, EA_8BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_adds, EA_4BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_subs, EA_8BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, 0); // == mov
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, -1);
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, 0xfff);
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, -0xfff);
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, 0x1000);
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, 0xfff000);
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, 0xfffffffffffff000LL);
    theEmitter->emitIns_R_R_I(INS_subs, EA_4BYTE, REG_R8, REG_R9, 0xffffffffff800000LL);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_I cmp/txt
    //

    // cmp
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 0);

    // CMP (shifted register)
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 31, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 32, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 33, INS_OPTS_ASR);

    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 21, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 22, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 23, INS_OPTS_ASR);

    // TST (shifted register)
    theEmitter->emitIns_R_R_I(INS_tst, EA_8BYTE, REG_R8, REG_R9, 31, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_I(INS_tst, EA_8BYTE, REG_R8, REG_R9, 32, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_I(INS_tst, EA_8BYTE, REG_R8, REG_R9, 33, INS_OPTS_ASR);
    theEmitter->emitIns_R_R_I(INS_tst, EA_8BYTE, REG_R8, REG_R9, 34, INS_OPTS_ROR);

    theEmitter->emitIns_R_R_I(INS_tst, EA_4BYTE, REG_R8, REG_R9, 21, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_I(INS_tst, EA_4BYTE, REG_R8, REG_R9, 22, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_I(INS_tst, EA_4BYTE, REG_R8, REG_R9, 23, INS_OPTS_ASR);
    theEmitter->emitIns_R_R_I(INS_tst, EA_4BYTE, REG_R8, REG_R9, 24, INS_OPTS_ROR);

    // CMP (extended register)
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_UXTB);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_UXTH);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_UXTW); // "cmp x8, x9, UXTW"; msdis
                                                                                    // disassembles this "cmp x8,x9",
                                                                                    // which looks like an msdis issue.
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_UXTX);

    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_SXTB);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_SXTH);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 0, INS_OPTS_SXTX);

    // CMP 64-bit (extended register) and left shift
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 1, INS_OPTS_UXTB);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 2, INS_OPTS_UXTH);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 3, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 4, INS_OPTS_UXTX);

    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 1, INS_OPTS_SXTB);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 2, INS_OPTS_SXTH);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 3, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_8BYTE, REG_R8, REG_R9, 4, INS_OPTS_SXTX);

    // CMP 32-bit (extended register) and left shift
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 0, INS_OPTS_UXTB);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 2, INS_OPTS_UXTH);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 4, INS_OPTS_UXTW);

    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 0, INS_OPTS_SXTB);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 2, INS_OPTS_SXTH);
    theEmitter->emitIns_R_R_I(INS_cmp, EA_4BYTE, REG_R8, REG_R9, 4, INS_OPTS_SXTW);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R(INS_lsl, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_lsr, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_asr, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ror, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_adc, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_adcs, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_sbc, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_sbcs, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_udiv, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_sdiv, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_mul, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_mneg, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_smull, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_smnegl, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_smulh, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_umull, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_umnegl, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_umulh, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_lslv, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_lsrv, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_asrv, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_rorv, EA_8BYTE, REG_R8, REG_R9, REG_R10);

    theEmitter->emitIns_R_R_R(INS_lsl, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_lsr, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_asr, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ror, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_adc, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_adcs, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_sbc, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_sbcs, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_udiv, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_sdiv, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_mul, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_mneg, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_smull, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_smnegl, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_smulh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_umull, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_umnegl, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_umulh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_lslv, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_lsrv, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_asrv, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_rorv, EA_4BYTE, REG_R8, REG_R9, REG_R10);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // ARMv8.1 LSE Atomics
    //
    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R(INS_casb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casab, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casalb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_caslb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_cash, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casah, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casalh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_caslh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_cas, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casa, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casal, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casl, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_cas, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casa, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casal, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_casl, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddab, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddalb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddlb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddah, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddalh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddlh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldadd, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldadda, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddal, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddl, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldadd, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldadda, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddal, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_ldaddl, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpab, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpalb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swplb, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swph, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpah, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpalh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swplh, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swp, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpa, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpal, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpl, EA_4BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swp, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpa, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpal, EA_8BYTE, REG_R8, REG_R9, REG_R10);
    theEmitter->emitIns_R_R_R(INS_swpl, EA_8BYTE, REG_R8, REG_R9, REG_R10);

    theEmitter->emitIns_R_R(INS_staddb, EA_4BYTE, REG_R8, REG_R10);
    theEmitter->emitIns_R_R(INS_staddlb, EA_4BYTE, REG_R8, REG_R10);
    theEmitter->emitIns_R_R(INS_staddh, EA_4BYTE, REG_R8, REG_R10);
    theEmitter->emitIns_R_R(INS_staddlh, EA_4BYTE, REG_R8, REG_R10);
    theEmitter->emitIns_R_R(INS_stadd, EA_4BYTE, REG_R8, REG_R10);
    theEmitter->emitIns_R_R(INS_staddl, EA_4BYTE, REG_R8, REG_R10);
    theEmitter->emitIns_R_R(INS_stadd, EA_8BYTE, REG_R8, REG_R10);
    theEmitter->emitIns_R_R(INS_staddl, EA_8BYTE, REG_R8, REG_R10);
#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_I_I
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_I_I(INS_sbfm, EA_8BYTE, REG_R2, REG_R3, 4, 39);
    theEmitter->emitIns_R_R_I_I(INS_bfm, EA_8BYTE, REG_R1, REG_R5, 20, 23);
    theEmitter->emitIns_R_R_I_I(INS_ubfm, EA_8BYTE, REG_R8, REG_R9, 36, 7);

    theEmitter->emitIns_R_R_I_I(INS_sbfiz, EA_8BYTE, REG_R2, REG_R3, 7, 37);
    theEmitter->emitIns_R_R_I_I(INS_bfi, EA_8BYTE, REG_R1, REG_R5, 23, 21);
    theEmitter->emitIns_R_R_I_I(INS_ubfiz, EA_8BYTE, REG_R8, REG_R9, 39, 5);

    theEmitter->emitIns_R_R_I_I(INS_sbfx, EA_8BYTE, REG_R2, REG_R3, 10, 24);
    theEmitter->emitIns_R_R_I_I(INS_bfxil, EA_8BYTE, REG_R1, REG_R5, 26, 16);
    theEmitter->emitIns_R_R_I_I(INS_ubfx, EA_8BYTE, REG_R8, REG_R9, 42, 8);

    theEmitter->emitIns_R_R_I_I(INS_sbfm, EA_4BYTE, REG_R2, REG_R3, 4, 19);
    theEmitter->emitIns_R_R_I_I(INS_bfm, EA_4BYTE, REG_R1, REG_R5, 10, 13);
    theEmitter->emitIns_R_R_I_I(INS_ubfm, EA_4BYTE, REG_R8, REG_R9, 16, 7);

    theEmitter->emitIns_R_R_I_I(INS_sbfiz, EA_4BYTE, REG_R2, REG_R3, 5, 17);
    theEmitter->emitIns_R_R_I_I(INS_bfi, EA_4BYTE, REG_R1, REG_R5, 13, 11);
    theEmitter->emitIns_R_R_I_I(INS_ubfiz, EA_4BYTE, REG_R8, REG_R9, 19, 5);

    theEmitter->emitIns_R_R_I_I(INS_sbfx, EA_4BYTE, REG_R2, REG_R3, 3, 14);
    theEmitter->emitIns_R_R_I_I(INS_bfxil, EA_4BYTE, REG_R1, REG_R5, 11, 9);
    theEmitter->emitIns_R_R_I_I(INS_ubfx, EA_4BYTE, REG_R8, REG_R9, 22, 8);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R_I
    //

    genDefineTempLabel(genCreateTempLabel());

    // ADD (extended register)
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTB);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTH);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTB);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTH);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTX);

    // ADD (extended register) and left shift
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTB);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTH);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTB);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTH);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTX);

    // ADD (shifted register)
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 31, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 32, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_R_I(INS_add, EA_8BYTE, REG_R8, REG_R9, REG_R10, 33, INS_OPTS_ASR);

    // EXTR (extract field from register pair)
    theEmitter->emitIns_R_R_R_I(INS_extr, EA_8BYTE, REG_R8, REG_R9, REG_R10, 1);
    theEmitter->emitIns_R_R_R_I(INS_extr, EA_8BYTE, REG_R8, REG_R9, REG_R10, 31);
    theEmitter->emitIns_R_R_R_I(INS_extr, EA_8BYTE, REG_R8, REG_R9, REG_R10, 32);
    theEmitter->emitIns_R_R_R_I(INS_extr, EA_8BYTE, REG_R8, REG_R9, REG_R10, 63);

    theEmitter->emitIns_R_R_R_I(INS_extr, EA_4BYTE, REG_R8, REG_R9, REG_R10, 1);
    theEmitter->emitIns_R_R_R_I(INS_extr, EA_4BYTE, REG_R8, REG_R9, REG_R10, 31);

    // SUB (extended register)
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTB);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTH);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTB);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTH);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0, INS_OPTS_SXTX);

    // SUB (extended register) and left shift
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTB);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTH);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTB);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTH);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_SXTX);

    // SUB (shifted register)
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 27, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 28, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_R_I(INS_sub, EA_4BYTE, REG_R8, REG_R9, REG_R10, 29, INS_OPTS_ASR);

    // bit operations
    theEmitter->emitIns_R_R_R_I(INS_and, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_ands, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_eor, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_orr, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_bic, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_bics, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_eon, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_orn, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);

    theEmitter->emitIns_R_R_R_I(INS_and, EA_8BYTE, REG_R8, REG_R9, REG_R10, 1, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_I(INS_ands, EA_8BYTE, REG_R8, REG_R9, REG_R10, 2, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_R_I(INS_eor, EA_8BYTE, REG_R8, REG_R9, REG_R10, 3, INS_OPTS_ASR);
    theEmitter->emitIns_R_R_R_I(INS_orr, EA_8BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_ROR);
    theEmitter->emitIns_R_R_R_I(INS_bic, EA_8BYTE, REG_R8, REG_R9, REG_R10, 5, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_I(INS_bics, EA_8BYTE, REG_R8, REG_R9, REG_R10, 6, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_R_I(INS_eon, EA_8BYTE, REG_R8, REG_R9, REG_R10, 7, INS_OPTS_ASR);
    theEmitter->emitIns_R_R_R_I(INS_orn, EA_8BYTE, REG_R8, REG_R9, REG_R10, 8, INS_OPTS_ROR);

    theEmitter->emitIns_R_R_R_I(INS_and, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_ands, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_eor, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_orr, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_bic, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_bics, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_eon, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_orn, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);

    theEmitter->emitIns_R_R_R_I(INS_and, EA_4BYTE, REG_R8, REG_R9, REG_R10, 1, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_I(INS_ands, EA_4BYTE, REG_R8, REG_R9, REG_R10, 2, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_R_I(INS_eor, EA_4BYTE, REG_R8, REG_R9, REG_R10, 3, INS_OPTS_ASR);
    theEmitter->emitIns_R_R_R_I(INS_orr, EA_4BYTE, REG_R8, REG_R9, REG_R10, 4, INS_OPTS_ROR);
    theEmitter->emitIns_R_R_R_I(INS_bic, EA_4BYTE, REG_R8, REG_R9, REG_R10, 5, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_I(INS_bics, EA_4BYTE, REG_R8, REG_R9, REG_R10, 6, INS_OPTS_LSR);
    theEmitter->emitIns_R_R_R_I(INS_eon, EA_4BYTE, REG_R8, REG_R9, REG_R10, 7, INS_OPTS_ASR);
    theEmitter->emitIns_R_R_R_I(INS_orn, EA_4BYTE, REG_R8, REG_R9, REG_R10, 8, INS_OPTS_ROR);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R_I  -- load/store pair
    //

    theEmitter->emitIns_R_R_R_I(INS_ldnp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldnp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 8);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 8);

    theEmitter->emitIns_R_R_R_I(INS_ldnp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldnp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 8);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 8);

    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 16);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 16);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_PRE_INDEX);

    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 16);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_R8, REG_R9, REG_SP, 16);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_4BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_4BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_PRE_INDEX);

    theEmitter->emitIns_R_R_R_I(INS_ldpsw, EA_4BYTE, REG_R8, REG_R9, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldpsw, EA_4BYTE, REG_R8, REG_R9, REG_R10, 16);
    theEmitter->emitIns_R_R_R_I(INS_ldpsw, EA_4BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_ldpsw, EA_4BYTE, REG_R8, REG_R9, REG_R10, 16, INS_OPTS_PRE_INDEX);

    // SP and ZR tests
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_ZR, REG_R1, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_R0, REG_ZR, REG_SP, 16);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_ZR, REG_R1, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_R0, REG_ZR, REG_SP, 16);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_ZR, REG_ZR, REG_SP, 16, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_ZR, REG_ZR, REG_R8, 16, INS_OPTS_PRE_INDEX);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R_Ext    -- load/store shifted/extend
    //

    genDefineTempLabel(genCreateTempLabel());

    // LDR (register)
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 3);

    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 2);

    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 1);

    theEmitter->emitIns_R_R_R_Ext(INS_ldrb, EA_1BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);

    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsw, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 2);

    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_4BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_8BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsh, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 1);

    theEmitter->emitIns_R_R_R_Ext(INS_ldrsb, EA_4BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsb, EA_8BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsb, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsb, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsb, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldrsb, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);

    // STR (register)
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_8BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 3);

    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_str, EA_4BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 2);

    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_LSL, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_strh, EA_2BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX, 1);

    theEmitter->emitIns_R_R_R_Ext(INS_strb, EA_1BYTE, REG_R8, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_strb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_strb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_strb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_strb, EA_1BYTE, REG_R8, REG_SP, REG_R9, INS_OPTS_UXTX);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R_R
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R_R(INS_madd, EA_4BYTE, REG_R0, REG_R12, REG_R27, REG_R10);
    theEmitter->emitIns_R_R_R_R(INS_msub, EA_4BYTE, REG_R1, REG_R13, REG_R28, REG_R11);
    theEmitter->emitIns_R_R_R_R(INS_smaddl, EA_4BYTE, REG_R2, REG_R14, REG_R0, REG_R12);
    theEmitter->emitIns_R_R_R_R(INS_smsubl, EA_4BYTE, REG_R3, REG_R15, REG_R1, REG_R13);
    theEmitter->emitIns_R_R_R_R(INS_umaddl, EA_4BYTE, REG_R4, REG_R19, REG_R2, REG_R14);
    theEmitter->emitIns_R_R_R_R(INS_umsubl, EA_4BYTE, REG_R5, REG_R20, REG_R3, REG_R15);

    theEmitter->emitIns_R_R_R_R(INS_madd, EA_8BYTE, REG_R6, REG_R21, REG_R4, REG_R19);
    theEmitter->emitIns_R_R_R_R(INS_msub, EA_8BYTE, REG_R7, REG_R22, REG_R5, REG_R20);
    theEmitter->emitIns_R_R_R_R(INS_smaddl, EA_8BYTE, REG_R8, REG_R23, REG_R6, REG_R21);
    theEmitter->emitIns_R_R_R_R(INS_smsubl, EA_8BYTE, REG_R9, REG_R24, REG_R7, REG_R22);
    theEmitter->emitIns_R_R_R_R(INS_umaddl, EA_8BYTE, REG_R10, REG_R25, REG_R8, REG_R23);
    theEmitter->emitIns_R_R_R_R(INS_umsubl, EA_8BYTE, REG_R11, REG_R26, REG_R9, REG_R24);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    // R_COND
    //

    // cset reg, cond
    theEmitter->emitIns_R_COND(INS_cset, EA_8BYTE, REG_R9, INS_COND_EQ); // eq
    theEmitter->emitIns_R_COND(INS_cset, EA_4BYTE, REG_R8, INS_COND_NE); // ne
    theEmitter->emitIns_R_COND(INS_cset, EA_4BYTE, REG_R7, INS_COND_HS); // hs
    theEmitter->emitIns_R_COND(INS_cset, EA_8BYTE, REG_R6, INS_COND_LO); // lo
    theEmitter->emitIns_R_COND(INS_cset, EA_8BYTE, REG_R5, INS_COND_MI); // mi
    theEmitter->emitIns_R_COND(INS_cset, EA_4BYTE, REG_R4, INS_COND_PL); // pl
    theEmitter->emitIns_R_COND(INS_cset, EA_4BYTE, REG_R3, INS_COND_VS); // vs
    theEmitter->emitIns_R_COND(INS_cset, EA_8BYTE, REG_R2, INS_COND_VC); // vc
    theEmitter->emitIns_R_COND(INS_cset, EA_8BYTE, REG_R1, INS_COND_HI); // hi
    theEmitter->emitIns_R_COND(INS_cset, EA_4BYTE, REG_R0, INS_COND_LS); // ls
    theEmitter->emitIns_R_COND(INS_cset, EA_4BYTE, REG_R9, INS_COND_GE); // ge
    theEmitter->emitIns_R_COND(INS_cset, EA_8BYTE, REG_R8, INS_COND_LT); // lt
    theEmitter->emitIns_R_COND(INS_cset, EA_8BYTE, REG_R7, INS_COND_GT); // gt
    theEmitter->emitIns_R_COND(INS_cset, EA_4BYTE, REG_R6, INS_COND_LE); // le

    // csetm reg, cond
    theEmitter->emitIns_R_COND(INS_csetm, EA_4BYTE, REG_R9, INS_COND_EQ); // eq
    theEmitter->emitIns_R_COND(INS_csetm, EA_8BYTE, REG_R8, INS_COND_NE); // ne
    theEmitter->emitIns_R_COND(INS_csetm, EA_8BYTE, REG_R7, INS_COND_HS); // hs
    theEmitter->emitIns_R_COND(INS_csetm, EA_4BYTE, REG_R6, INS_COND_LO); // lo
    theEmitter->emitIns_R_COND(INS_csetm, EA_4BYTE, REG_R5, INS_COND_MI); // mi
    theEmitter->emitIns_R_COND(INS_csetm, EA_8BYTE, REG_R4, INS_COND_PL); // pl
    theEmitter->emitIns_R_COND(INS_csetm, EA_8BYTE, REG_R3, INS_COND_VS); // vs
    theEmitter->emitIns_R_COND(INS_csetm, EA_4BYTE, REG_R2, INS_COND_VC); // vc
    theEmitter->emitIns_R_COND(INS_csetm, EA_4BYTE, REG_R1, INS_COND_HI); // hi
    theEmitter->emitIns_R_COND(INS_csetm, EA_8BYTE, REG_R0, INS_COND_LS); // ls
    theEmitter->emitIns_R_COND(INS_csetm, EA_8BYTE, REG_R9, INS_COND_GE); // ge
    theEmitter->emitIns_R_COND(INS_csetm, EA_4BYTE, REG_R8, INS_COND_LT); // lt
    theEmitter->emitIns_R_COND(INS_csetm, EA_4BYTE, REG_R7, INS_COND_GT); // gt
    theEmitter->emitIns_R_COND(INS_csetm, EA_8BYTE, REG_R6, INS_COND_LE); // le

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    // R_R_COND
    //

    // cinc reg, reg, cond
    // cinv reg, reg, cond
    // cneg reg, reg, cond
    theEmitter->emitIns_R_R_COND(INS_cinc, EA_8BYTE, REG_R0, REG_R4, INS_COND_EQ); // eq
    theEmitter->emitIns_R_R_COND(INS_cinv, EA_4BYTE, REG_R1, REG_R5, INS_COND_NE); // ne
    theEmitter->emitIns_R_R_COND(INS_cneg, EA_4BYTE, REG_R2, REG_R6, INS_COND_HS); // hs
    theEmitter->emitIns_R_R_COND(INS_cinc, EA_8BYTE, REG_R3, REG_R7, INS_COND_LO); // lo
    theEmitter->emitIns_R_R_COND(INS_cinv, EA_4BYTE, REG_R4, REG_R8, INS_COND_MI); // mi
    theEmitter->emitIns_R_R_COND(INS_cneg, EA_8BYTE, REG_R5, REG_R9, INS_COND_PL); // pl
    theEmitter->emitIns_R_R_COND(INS_cinc, EA_8BYTE, REG_R6, REG_R0, INS_COND_VS); // vs
    theEmitter->emitIns_R_R_COND(INS_cinv, EA_4BYTE, REG_R7, REG_R1, INS_COND_VC); // vc
    theEmitter->emitIns_R_R_COND(INS_cneg, EA_8BYTE, REG_R8, REG_R2, INS_COND_HI); // hi
    theEmitter->emitIns_R_R_COND(INS_cinc, EA_4BYTE, REG_R9, REG_R3, INS_COND_LS); // ls
    theEmitter->emitIns_R_R_COND(INS_cinv, EA_4BYTE, REG_R0, REG_R4, INS_COND_GE); // ge
    theEmitter->emitIns_R_R_COND(INS_cneg, EA_8BYTE, REG_R2, REG_R5, INS_COND_LT); // lt
    theEmitter->emitIns_R_R_COND(INS_cinc, EA_4BYTE, REG_R2, REG_R6, INS_COND_GT); // gt
    theEmitter->emitIns_R_R_COND(INS_cinv, EA_8BYTE, REG_R3, REG_R7, INS_COND_LE); // le

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    // R_R_R_COND
    //

    // csel  reg, reg, reg, cond
    // csinc reg, reg, reg, cond
    // csinv reg, reg, reg, cond
    // csneg reg, reg, reg, cond
    theEmitter->emitIns_R_R_R_COND(INS_csel, EA_8BYTE, REG_R0, REG_R4, REG_R8, INS_COND_EQ);  // eq
    theEmitter->emitIns_R_R_R_COND(INS_csinc, EA_4BYTE, REG_R1, REG_R5, REG_R9, INS_COND_NE); // ne
    theEmitter->emitIns_R_R_R_COND(INS_csinv, EA_4BYTE, REG_R2, REG_R6, REG_R0, INS_COND_HS); // hs
    theEmitter->emitIns_R_R_R_COND(INS_csneg, EA_8BYTE, REG_R3, REG_R7, REG_R1, INS_COND_LO); // lo
    theEmitter->emitIns_R_R_R_COND(INS_csel, EA_4BYTE, REG_R4, REG_R8, REG_R2, INS_COND_MI);  // mi
    theEmitter->emitIns_R_R_R_COND(INS_csinc, EA_8BYTE, REG_R5, REG_R9, REG_R3, INS_COND_PL); // pl
    theEmitter->emitIns_R_R_R_COND(INS_csinv, EA_8BYTE, REG_R6, REG_R0, REG_R4, INS_COND_VS); // vs
    theEmitter->emitIns_R_R_R_COND(INS_csneg, EA_4BYTE, REG_R7, REG_R1, REG_R5, INS_COND_VC); // vc
    theEmitter->emitIns_R_R_R_COND(INS_csel, EA_8BYTE, REG_R8, REG_R2, REG_R6, INS_COND_HI);  // hi
    theEmitter->emitIns_R_R_R_COND(INS_csinc, EA_4BYTE, REG_R9, REG_R3, REG_R7, INS_COND_LS); // ls
    theEmitter->emitIns_R_R_R_COND(INS_csinv, EA_4BYTE, REG_R0, REG_R4, REG_R8, INS_COND_GE); // ge
    theEmitter->emitIns_R_R_R_COND(INS_csneg, EA_8BYTE, REG_R2, REG_R5, REG_R9, INS_COND_LT); // lt
    theEmitter->emitIns_R_R_R_COND(INS_csel, EA_4BYTE, REG_R2, REG_R6, REG_R0, INS_COND_GT);  // gt
    theEmitter->emitIns_R_R_R_COND(INS_csinc, EA_8BYTE, REG_R3, REG_R7, REG_R1, INS_COND_LE); // le

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    // R_R_FLAGS_COND
    //

    // ccmp reg1, reg2, nzcv, cond
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R9, REG_R3, INS_FLAGS_V, INS_COND_EQ);    // eq
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R8, REG_R2, INS_FLAGS_C, INS_COND_NE);    // ne
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R7, REG_R1, INS_FLAGS_Z, INS_COND_HS);    // hs
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R6, REG_R0, INS_FLAGS_N, INS_COND_LO);    // lo
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R5, REG_R3, INS_FLAGS_CV, INS_COND_MI);   // mi
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R4, REG_R2, INS_FLAGS_ZV, INS_COND_PL);   // pl
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R3, REG_R1, INS_FLAGS_ZC, INS_COND_VS);   // vs
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R2, REG_R0, INS_FLAGS_NV, INS_COND_VC);   // vc
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R1, REG_R3, INS_FLAGS_NC, INS_COND_HI);   // hi
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R0, REG_R2, INS_FLAGS_NZ, INS_COND_LS);   // ls
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R9, REG_R1, INS_FLAGS_NONE, INS_COND_GE); // ge
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R8, REG_R0, INS_FLAGS_NZV, INS_COND_LT);  // lt
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R7, REG_R3, INS_FLAGS_NZC, INS_COND_GT);  // gt
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R6, REG_R2, INS_FLAGS_NZCV, INS_COND_LE); // le

    // ccmp reg1, imm, nzcv, cond
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R9, 3, INS_FLAGS_V, INS_COND_EQ);     // eq
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R8, 2, INS_FLAGS_C, INS_COND_NE);     // ne
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R7, 1, INS_FLAGS_Z, INS_COND_HS);     // hs
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R6, 0, INS_FLAGS_N, INS_COND_LO);     // lo
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R5, 31, INS_FLAGS_CV, INS_COND_MI);   // mi
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R4, 28, INS_FLAGS_ZV, INS_COND_PL);   // pl
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R3, 25, INS_FLAGS_ZC, INS_COND_VS);   // vs
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R2, 22, INS_FLAGS_NV, INS_COND_VC);   // vc
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R1, 19, INS_FLAGS_NC, INS_COND_HI);   // hi
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R0, 16, INS_FLAGS_NZ, INS_COND_LS);   // ls
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R9, 13, INS_FLAGS_NONE, INS_COND_GE); // ge
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R8, 10, INS_FLAGS_NZV, INS_COND_LT);  // lt
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R7, 7, INS_FLAGS_NZC, INS_COND_GT);   // gt
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R6, 4, INS_FLAGS_NZCV, INS_COND_LE);  // le

    // ccmp reg1, imm, nzcv, cond  -- encoded as ccmn
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R9, -3, INS_FLAGS_V, INS_COND_EQ);     // eq
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R8, -2, INS_FLAGS_C, INS_COND_NE);     // ne
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R7, -1, INS_FLAGS_Z, INS_COND_HS);     // hs
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R6, -5, INS_FLAGS_N, INS_COND_LO);     // lo
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R5, -31, INS_FLAGS_CV, INS_COND_MI);   // mi
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R4, -28, INS_FLAGS_ZV, INS_COND_PL);   // pl
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R3, -25, INS_FLAGS_ZC, INS_COND_VS);   // vs
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R2, -22, INS_FLAGS_NV, INS_COND_VC);   // vc
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R1, -19, INS_FLAGS_NC, INS_COND_HI);   // hi
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R0, -16, INS_FLAGS_NZ, INS_COND_LS);   // ls
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R9, -13, INS_FLAGS_NONE, INS_COND_GE); // ge
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R8, -10, INS_FLAGS_NZV, INS_COND_LT);  // lt
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_8BYTE, REG_R7, -7, INS_FLAGS_NZC, INS_COND_GT);   // gt
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmp, EA_4BYTE, REG_R6, -4, INS_FLAGS_NZCV, INS_COND_LE);  // le

    // ccmn reg1, reg2, nzcv, cond
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R9, REG_R3, INS_FLAGS_V, INS_COND_EQ);    // eq
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R8, REG_R2, INS_FLAGS_C, INS_COND_NE);    // ne
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R7, REG_R1, INS_FLAGS_Z, INS_COND_HS);    // hs
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R6, REG_R0, INS_FLAGS_N, INS_COND_LO);    // lo
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R5, REG_R3, INS_FLAGS_CV, INS_COND_MI);   // mi
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R4, REG_R2, INS_FLAGS_ZV, INS_COND_PL);   // pl
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R3, REG_R1, INS_FLAGS_ZC, INS_COND_VS);   // vs
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R2, REG_R0, INS_FLAGS_NV, INS_COND_VC);   // vc
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R1, REG_R3, INS_FLAGS_NC, INS_COND_HI);   // hi
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R0, REG_R2, INS_FLAGS_NZ, INS_COND_LS);   // ls
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R9, REG_R1, INS_FLAGS_NONE, INS_COND_GE); // ge
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R8, REG_R0, INS_FLAGS_NZV, INS_COND_LT);  // lt
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R7, REG_R3, INS_FLAGS_NZC, INS_COND_GT);  // gt
    theEmitter->emitIns_R_R_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R6, REG_R2, INS_FLAGS_NZCV, INS_COND_LE); // le

    // ccmn reg1, imm, nzcv, cond
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R9, 3, INS_FLAGS_V, INS_COND_EQ);     // eq
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R8, 2, INS_FLAGS_C, INS_COND_NE);     // ne
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R7, 1, INS_FLAGS_Z, INS_COND_HS);     // hs
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R6, 0, INS_FLAGS_N, INS_COND_LO);     // lo
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R5, 31, INS_FLAGS_CV, INS_COND_MI);   // mi
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R4, 28, INS_FLAGS_ZV, INS_COND_PL);   // pl
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R3, 25, INS_FLAGS_ZC, INS_COND_VS);   // vs
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R2, 22, INS_FLAGS_NV, INS_COND_VC);   // vc
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R1, 19, INS_FLAGS_NC, INS_COND_HI);   // hi
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R0, 16, INS_FLAGS_NZ, INS_COND_LS);   // ls
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R9, 13, INS_FLAGS_NONE, INS_COND_GE); // ge
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R8, 10, INS_FLAGS_NZV, INS_COND_LT);  // lt
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_8BYTE, REG_R7, 7, INS_FLAGS_NZC, INS_COND_GT);   // gt
    theEmitter->emitIns_R_I_FLAGS_COND(INS_ccmn, EA_4BYTE, REG_R6, 4, INS_FLAGS_NZCV, INS_COND_LE);  // le

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // Branch to register
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R(INS_br, EA_PTRSIZE, REG_R8);
    theEmitter->emitIns_R(INS_blr, EA_PTRSIZE, REG_R9);
    theEmitter->emitIns_R(INS_ret, EA_PTRSIZE, REG_R8);
    theEmitter->emitIns_R(INS_ret, EA_PTRSIZE, REG_LR);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // Misc
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_I(INS_brk, EA_PTRSIZE, 0);
    theEmitter->emitIns_I(INS_brk, EA_PTRSIZE, 65535);

    theEmitter->emitIns_BARR(INS_dsb, INS_BARRIER_OSHLD);
    theEmitter->emitIns_BARR(INS_dmb, INS_BARRIER_OSHST);
    theEmitter->emitIns_BARR(INS_isb, INS_BARRIER_OSH);

    theEmitter->emitIns_BARR(INS_dmb, INS_BARRIER_NSHLD);
    theEmitter->emitIns_BARR(INS_isb, INS_BARRIER_NSHST);
    theEmitter->emitIns_BARR(INS_dsb, INS_BARRIER_NSH);

    theEmitter->emitIns_BARR(INS_isb, INS_BARRIER_ISHLD);
    theEmitter->emitIns_BARR(INS_dsb, INS_BARRIER_ISHST);
    theEmitter->emitIns_BARR(INS_dmb, INS_BARRIER_ISH);

    theEmitter->emitIns_BARR(INS_dsb, INS_BARRIER_LD);
    theEmitter->emitIns_BARR(INS_dmb, INS_BARRIER_ST);
    theEmitter->emitIns_BARR(INS_isb, INS_BARRIER_SY);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    ////////////////////////////////////////////////////////////////////////////////
    //
    // SIMD and Floating point
    //
    ////////////////////////////////////////////////////////////////////////////////

    //
    // Load/Stores vector register
    //

    genDefineTempLabel(genCreateTempLabel());

    // ldr/str Vt, [reg]
    theEmitter->emitIns_R_R(INS_ldr, EA_8BYTE, REG_V1, REG_R9);
    theEmitter->emitIns_R_R(INS_str, EA_8BYTE, REG_V2, REG_R8);
    theEmitter->emitIns_R_R(INS_ldr, EA_4BYTE, REG_V3, REG_R7);
    theEmitter->emitIns_R_R(INS_str, EA_4BYTE, REG_V4, REG_R6);
    theEmitter->emitIns_R_R(INS_ldr, EA_2BYTE, REG_V5, REG_R5);
    theEmitter->emitIns_R_R(INS_str, EA_2BYTE, REG_V6, REG_R4);
    theEmitter->emitIns_R_R(INS_ldr, EA_1BYTE, REG_V7, REG_R3);
    theEmitter->emitIns_R_R(INS_str, EA_1BYTE, REG_V8, REG_R2);
    theEmitter->emitIns_R_R(INS_ldr, EA_16BYTE, REG_V9, REG_R1);
    theEmitter->emitIns_R_R(INS_str, EA_16BYTE, REG_V10, REG_R0);

    // ldr/str Vt, [reg+cns]        -- scaled
    theEmitter->emitIns_R_R_I(INS_ldr, EA_1BYTE, REG_V8, REG_R9, 1);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_2BYTE, REG_V8, REG_R9, 2);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_V8, REG_R9, 4);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_8BYTE, REG_V8, REG_R9, 8);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_16BYTE, REG_V8, REG_R9, 16);

    theEmitter->emitIns_R_R_I(INS_ldr, EA_1BYTE, REG_V7, REG_R10, 1);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_2BYTE, REG_V7, REG_R10, 2);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_V7, REG_R10, 4);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_8BYTE, REG_V7, REG_R10, 8);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_16BYTE, REG_V7, REG_R10, 16);

    // ldr/str Vt, [reg],cns        -- post-indexed (unscaled)
    // ldr/str Vt, [reg+cns]!       -- post-indexed (unscaled)
    theEmitter->emitIns_R_R_I(INS_ldr, EA_1BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_2BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_8BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_16BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);

    theEmitter->emitIns_R_R_I(INS_ldr, EA_1BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_2BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_4BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_8BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_ldr, EA_16BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);

    theEmitter->emitIns_R_R_I(INS_str, EA_1BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_2BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_4BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_8BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_16BYTE, REG_V8, REG_R9, 1, INS_OPTS_POST_INDEX);

    theEmitter->emitIns_R_R_I(INS_str, EA_1BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_2BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_4BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_8BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_I(INS_str, EA_16BYTE, REG_V8, REG_R9, 1, INS_OPTS_PRE_INDEX);

    theEmitter->emitIns_R_R_I(INS_ldur, EA_1BYTE, REG_V8, REG_R9, 2);
    theEmitter->emitIns_R_R_I(INS_ldur, EA_2BYTE, REG_V8, REG_R9, 3);
    theEmitter->emitIns_R_R_I(INS_ldur, EA_4BYTE, REG_V8, REG_R9, 5);
    theEmitter->emitIns_R_R_I(INS_ldur, EA_8BYTE, REG_V8, REG_R9, 9);
    theEmitter->emitIns_R_R_I(INS_ldur, EA_16BYTE, REG_V8, REG_R9, 17);

    theEmitter->emitIns_R_R_I(INS_stur, EA_1BYTE, REG_V7, REG_R10, 2);
    theEmitter->emitIns_R_R_I(INS_stur, EA_2BYTE, REG_V7, REG_R10, 3);
    theEmitter->emitIns_R_R_I(INS_stur, EA_4BYTE, REG_V7, REG_R10, 5);
    theEmitter->emitIns_R_R_I(INS_stur, EA_8BYTE, REG_V7, REG_R10, 9);
    theEmitter->emitIns_R_R_I(INS_stur, EA_16BYTE, REG_V7, REG_R10, 17);

    // load/store pair
    theEmitter->emitIns_R_R_R(INS_ldnp, EA_8BYTE, REG_V0, REG_V1, REG_R10);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_8BYTE, REG_V1, REG_V2, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldnp, EA_8BYTE, REG_V2, REG_V3, REG_R10, 8);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_8BYTE, REG_V3, REG_V4, REG_R10, 24);

    theEmitter->emitIns_R_R_R(INS_ldnp, EA_4BYTE, REG_V4, REG_V5, REG_SP);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_4BYTE, REG_V5, REG_V6, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldnp, EA_4BYTE, REG_V6, REG_V7, REG_SP, 4);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_4BYTE, REG_V7, REG_V8, REG_SP, 12);

    theEmitter->emitIns_R_R_R(INS_ldnp, EA_16BYTE, REG_V8, REG_V9, REG_R10);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_16BYTE, REG_V9, REG_V10, REG_R10, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldnp, EA_16BYTE, REG_V10, REG_V11, REG_R10, 16);
    theEmitter->emitIns_R_R_R_I(INS_stnp, EA_16BYTE, REG_V11, REG_V12, REG_R10, 48);

    theEmitter->emitIns_R_R_R(INS_ldp, EA_8BYTE, REG_V0, REG_V1, REG_R10);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_V1, REG_V2, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_V2, REG_V3, REG_SP, 8);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_V3, REG_V4, REG_R10, 16);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_V4, REG_V5, REG_R10, 24, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_V5, REG_V6, REG_SP, 32, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_8BYTE, REG_V6, REG_V7, REG_SP, 40, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_8BYTE, REG_V7, REG_V8, REG_R10, 48, INS_OPTS_PRE_INDEX);

    theEmitter->emitIns_R_R_R(INS_ldp, EA_4BYTE, REG_V0, REG_V1, REG_R10);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_V1, REG_V2, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_4BYTE, REG_V2, REG_V3, REG_SP, 4);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_V3, REG_V4, REG_R10, 8);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_4BYTE, REG_V4, REG_V5, REG_R10, 12, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_V5, REG_V6, REG_SP, 16, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_4BYTE, REG_V6, REG_V7, REG_SP, 20, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_4BYTE, REG_V7, REG_V8, REG_R10, 24, INS_OPTS_PRE_INDEX);

    theEmitter->emitIns_R_R_R(INS_ldp, EA_16BYTE, REG_V0, REG_V1, REG_R10);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_16BYTE, REG_V1, REG_V2, REG_SP, 0);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_16BYTE, REG_V2, REG_V3, REG_SP, 16);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_16BYTE, REG_V3, REG_V4, REG_R10, 32);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_16BYTE, REG_V4, REG_V5, REG_R10, 48, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_16BYTE, REG_V5, REG_V6, REG_SP, 64, INS_OPTS_POST_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_ldp, EA_16BYTE, REG_V6, REG_V7, REG_SP, 80, INS_OPTS_PRE_INDEX);
    theEmitter->emitIns_R_R_R_I(INS_stp, EA_16BYTE, REG_V7, REG_V8, REG_R10, 96, INS_OPTS_PRE_INDEX);

    // LDR (register)
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V1, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V2, REG_R7, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V3, REG_R7, REG_R9, INS_OPTS_LSL, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V4, REG_R7, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V5, REG_R7, REG_R9, INS_OPTS_SXTW, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V6, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V7, REG_R7, REG_R9, INS_OPTS_UXTW, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V8, REG_R7, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V9, REG_R7, REG_R9, INS_OPTS_SXTX, 3);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V10, REG_R7, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_8BYTE, REG_V11, REG_SP, REG_R9, INS_OPTS_UXTX, 3);

    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V1, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V2, REG_R7, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V3, REG_R7, REG_R9, INS_OPTS_LSL, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V4, REG_R7, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V5, REG_R7, REG_R9, INS_OPTS_SXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V6, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V7, REG_R7, REG_R9, INS_OPTS_UXTW, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V8, REG_R7, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V9, REG_R7, REG_R9, INS_OPTS_SXTX, 2);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V10, REG_R7, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_4BYTE, REG_V11, REG_SP, REG_R9, INS_OPTS_UXTX, 2);

    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V1, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V2, REG_R7, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V3, REG_R7, REG_R9, INS_OPTS_LSL, 4);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V4, REG_R7, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V5, REG_R7, REG_R9, INS_OPTS_SXTW, 4);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V6, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V7, REG_R7, REG_R9, INS_OPTS_UXTW, 4);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V8, REG_R7, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V9, REG_R7, REG_R9, INS_OPTS_SXTX, 4);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V10, REG_R7, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_16BYTE, REG_V11, REG_SP, REG_R9, INS_OPTS_UXTX, 4);

    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V1, REG_SP, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V2, REG_R7, REG_R9, INS_OPTS_LSL);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V3, REG_R7, REG_R9, INS_OPTS_LSL, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V4, REG_R7, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V5, REG_R7, REG_R9, INS_OPTS_SXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V6, REG_SP, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V7, REG_R7, REG_R9, INS_OPTS_UXTW, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V8, REG_R7, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V9, REG_R7, REG_R9, INS_OPTS_SXTX, 1);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V10, REG_R7, REG_R9, INS_OPTS_UXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_2BYTE, REG_V11, REG_SP, REG_R9, INS_OPTS_UXTX, 1);

    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_1BYTE, REG_V1, REG_R7, REG_R9);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_1BYTE, REG_V2, REG_SP, REG_R9, INS_OPTS_SXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_1BYTE, REG_V3, REG_R7, REG_R9, INS_OPTS_UXTW);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_1BYTE, REG_V4, REG_SP, REG_R9, INS_OPTS_SXTX);
    theEmitter->emitIns_R_R_R_Ext(INS_ldr, EA_1BYTE, REG_V5, REG_R7, REG_R9, INS_OPTS_UXTX);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R   mov and aliases for mov
    //

    // mov vector to vector
    theEmitter->emitIns_R_R(INS_mov, EA_8BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_mov, EA_16BYTE, REG_V2, REG_V3);

    theEmitter->emitIns_R_R(INS_mov, EA_4BYTE, REG_V12, REG_V13);
    theEmitter->emitIns_R_R(INS_mov, EA_2BYTE, REG_V14, REG_V15);
    theEmitter->emitIns_R_R(INS_mov, EA_1BYTE, REG_V16, REG_V17);

    // mov vector to general
    theEmitter->emitIns_R_R(INS_mov, EA_8BYTE, REG_R0, REG_V4);
    theEmitter->emitIns_R_R(INS_mov, EA_4BYTE, REG_R1, REG_V5);
    theEmitter->emitIns_R_R(INS_mov, EA_2BYTE, REG_R2, REG_V6);
    theEmitter->emitIns_R_R(INS_mov, EA_1BYTE, REG_R3, REG_V7);

    // mov general to vector
    theEmitter->emitIns_R_R(INS_mov, EA_8BYTE, REG_V8, REG_R4);
    theEmitter->emitIns_R_R(INS_mov, EA_4BYTE, REG_V9, REG_R5);
    theEmitter->emitIns_R_R(INS_mov, EA_2BYTE, REG_V10, REG_R6);
    theEmitter->emitIns_R_R(INS_mov, EA_1BYTE, REG_V11, REG_R7);

    // mov vector[index] to vector
    theEmitter->emitIns_R_R_I(INS_mov, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_mov, EA_4BYTE, REG_V2, REG_V3, 3);
    theEmitter->emitIns_R_R_I(INS_mov, EA_2BYTE, REG_V4, REG_V5, 7);
    theEmitter->emitIns_R_R_I(INS_mov, EA_1BYTE, REG_V6, REG_V7, 15);

    // mov to general from vector[index]
    theEmitter->emitIns_R_R_I(INS_mov, EA_8BYTE, REG_R8, REG_V16, 1);
    theEmitter->emitIns_R_R_I(INS_mov, EA_4BYTE, REG_R9, REG_V17, 2);
    theEmitter->emitIns_R_R_I(INS_mov, EA_2BYTE, REG_R10, REG_V18, 3);
    theEmitter->emitIns_R_R_I(INS_mov, EA_1BYTE, REG_R11, REG_V19, 4);

    // mov to vector[index] from general
    theEmitter->emitIns_R_R_I(INS_mov, EA_8BYTE, REG_V20, REG_R12, 1);
    theEmitter->emitIns_R_R_I(INS_mov, EA_4BYTE, REG_V21, REG_R13, 2);
    theEmitter->emitIns_R_R_I(INS_mov, EA_2BYTE, REG_V22, REG_R14, 6);
    theEmitter->emitIns_R_R_I(INS_mov, EA_1BYTE, REG_V23, REG_R15, 8);

    // mov vector[index] to vector[index2]
    theEmitter->emitIns_R_R_I_I(INS_mov, EA_8BYTE, REG_V8, REG_V9, 1, 0);
    theEmitter->emitIns_R_R_I_I(INS_mov, EA_4BYTE, REG_V10, REG_V11, 2, 1);
    theEmitter->emitIns_R_R_I_I(INS_mov, EA_2BYTE, REG_V12, REG_V13, 5, 2);
    theEmitter->emitIns_R_R_I_I(INS_mov, EA_1BYTE, REG_V14, REG_V15, 12, 3);

    //////////////////////////////////////////////////////////////////////////////////

    // mov/dup scalar
    theEmitter->emitIns_R_R_I(INS_dup, EA_8BYTE, REG_V24, REG_V25, 1);
    theEmitter->emitIns_R_R_I(INS_dup, EA_4BYTE, REG_V26, REG_V27, 3);
    theEmitter->emitIns_R_R_I(INS_dup, EA_2BYTE, REG_V28, REG_V29, 7);
    theEmitter->emitIns_R_R_I(INS_dup, EA_1BYTE, REG_V30, REG_V31, 15);

    // mov/ins vector element
    theEmitter->emitIns_R_R_I_I(INS_ins, EA_8BYTE, REG_V0, REG_V1, 0, 1);
    theEmitter->emitIns_R_R_I_I(INS_ins, EA_4BYTE, REG_V2, REG_V3, 2, 2);
    theEmitter->emitIns_R_R_I_I(INS_ins, EA_2BYTE, REG_V4, REG_V5, 4, 3);
    theEmitter->emitIns_R_R_I_I(INS_ins, EA_1BYTE, REG_V6, REG_V7, 8, 4);

    // umov to general from vector element
    theEmitter->emitIns_R_R_I(INS_umov, EA_8BYTE, REG_R0, REG_V8, 1);
    theEmitter->emitIns_R_R_I(INS_umov, EA_4BYTE, REG_R1, REG_V9, 2);
    theEmitter->emitIns_R_R_I(INS_umov, EA_2BYTE, REG_R2, REG_V10, 4);
    theEmitter->emitIns_R_R_I(INS_umov, EA_1BYTE, REG_R3, REG_V11, 8);

    // ins to vector element from general
    theEmitter->emitIns_R_R_I(INS_ins, EA_8BYTE, REG_V12, REG_R4, 1);
    theEmitter->emitIns_R_R_I(INS_ins, EA_4BYTE, REG_V13, REG_R5, 3);
    theEmitter->emitIns_R_R_I(INS_ins, EA_2BYTE, REG_V14, REG_R6, 7);
    theEmitter->emitIns_R_R_I(INS_ins, EA_1BYTE, REG_V15, REG_R7, 15);

    // smov to general from vector element
    theEmitter->emitIns_R_R_I(INS_smov, EA_4BYTE, REG_R5, REG_V17, 2);
    theEmitter->emitIns_R_R_I(INS_smov, EA_2BYTE, REG_R6, REG_V18, 4);
    theEmitter->emitIns_R_R_I(INS_smov, EA_1BYTE, REG_R7, REG_V19, 8);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_I   movi and mvni
    //

    // movi  imm8  (vector)
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V0, 0x00, INS_OPTS_8B);
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V1, 0xFF, INS_OPTS_8B);
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V2, 0x00, INS_OPTS_16B);
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V3, 0xFF, INS_OPTS_16B);

    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V4, 0x007F, INS_OPTS_4H);
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V5, 0x7F00, INS_OPTS_4H); // LSL  8
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V6, 0x003F, INS_OPTS_8H);
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V7, 0x3F00, INS_OPTS_8H); // LSL  8

    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V8, 0x1F, INS_OPTS_2S);
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V9, 0x1F00, INS_OPTS_2S);      // LSL  8
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V10, 0x1F0000, INS_OPTS_2S);   // LSL 16
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V11, 0x1F000000, INS_OPTS_2S); // LSL 24

    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V12, 0x1FFF, INS_OPTS_2S);   // MSL  8
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V13, 0x1FFFFF, INS_OPTS_2S); // MSL 16

    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V14, 0x37, INS_OPTS_4S);
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V15, 0x3700, INS_OPTS_4S);     // LSL  8
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V16, 0x370000, INS_OPTS_4S);   // LSL 16
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V17, 0x37000000, INS_OPTS_4S); // LSL 24

    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V18, 0x37FF, INS_OPTS_4S);   // MSL  8
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V19, 0x37FFFF, INS_OPTS_4S); // MSL 16

    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V20, 0xFF80, INS_OPTS_4H);  // mvni
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V21, 0xFFC0, INS_OPTS_8H); // mvni

    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V22, 0xFFFFFFE0, INS_OPTS_2S);  // mvni
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V23, 0xFFFFF0FF, INS_OPTS_4S); // mvni LSL  8
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V24, 0xFFF8FFFF, INS_OPTS_2S);  // mvni LSL 16
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V25, 0xFCFFFFFF, INS_OPTS_4S); // mvni LSL 24

    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V26, 0xFFFFFE00, INS_OPTS_2S);  // mvni MSL  8
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V27, 0xFFFC0000, INS_OPTS_4S); // mvni MSL 16

    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V28, 0x00FF00FF00FF00FF, INS_OPTS_1D);
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V29, 0x00FFFF0000FFFF00, INS_OPTS_2D);
    theEmitter->emitIns_R_I(INS_movi, EA_8BYTE, REG_V30, 0xFF000000FF000000);
    theEmitter->emitIns_R_I(INS_movi, EA_16BYTE, REG_V31, 0x0, INS_OPTS_2D);

    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V0, 0x0022, INS_OPTS_4H);
    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V1, 0x2200, INS_OPTS_4H); // LSL  8
    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V2, 0x0033, INS_OPTS_8H);
    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V3, 0x3300, INS_OPTS_8H); // LSL  8

    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V4, 0x42, INS_OPTS_2S);
    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V5, 0x4200, INS_OPTS_2S);     // LSL  8
    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V6, 0x420000, INS_OPTS_2S);   // LSL 16
    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V7, 0x42000000, INS_OPTS_2S); // LSL 24

    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V8, 0x42FF, INS_OPTS_2S);   // MSL  8
    theEmitter->emitIns_R_I(INS_mvni, EA_8BYTE, REG_V9, 0x42FFFF, INS_OPTS_2S); // MSL 16

    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V10, 0x5D, INS_OPTS_4S);
    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V11, 0x5D00, INS_OPTS_4S);     // LSL  8
    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V12, 0x5D0000, INS_OPTS_4S);   // LSL 16
    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V13, 0x5D000000, INS_OPTS_4S); // LSL 24

    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V14, 0x5DFF, INS_OPTS_4S);   // MSL  8
    theEmitter->emitIns_R_I(INS_mvni, EA_16BYTE, REG_V15, 0x5DFFFF, INS_OPTS_4S); // MSL 16

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_I   orr/bic vector immediate
    //

    theEmitter->emitIns_R_I(INS_orr, EA_8BYTE, REG_V0, 0x0022, INS_OPTS_4H);
    theEmitter->emitIns_R_I(INS_orr, EA_8BYTE, REG_V1, 0x2200, INS_OPTS_4H); // LSL  8
    theEmitter->emitIns_R_I(INS_orr, EA_16BYTE, REG_V2, 0x0033, INS_OPTS_8H);
    theEmitter->emitIns_R_I(INS_orr, EA_16BYTE, REG_V3, 0x3300, INS_OPTS_8H); // LSL  8

    theEmitter->emitIns_R_I(INS_orr, EA_8BYTE, REG_V4, 0x42, INS_OPTS_2S);
    theEmitter->emitIns_R_I(INS_orr, EA_8BYTE, REG_V5, 0x4200, INS_OPTS_2S);     // LSL  8
    theEmitter->emitIns_R_I(INS_orr, EA_8BYTE, REG_V6, 0x420000, INS_OPTS_2S);   // LSL 16
    theEmitter->emitIns_R_I(INS_orr, EA_8BYTE, REG_V7, 0x42000000, INS_OPTS_2S); // LSL 24

    theEmitter->emitIns_R_I(INS_orr, EA_16BYTE, REG_V10, 0x5D, INS_OPTS_4S);
    theEmitter->emitIns_R_I(INS_orr, EA_16BYTE, REG_V11, 0x5D00, INS_OPTS_4S);     // LSL  8
    theEmitter->emitIns_R_I(INS_orr, EA_16BYTE, REG_V12, 0x5D0000, INS_OPTS_4S);   // LSL 16
    theEmitter->emitIns_R_I(INS_orr, EA_16BYTE, REG_V13, 0x5D000000, INS_OPTS_4S); // LSL 24

    theEmitter->emitIns_R_I(INS_bic, EA_8BYTE, REG_V0, 0x0022, INS_OPTS_4H);
    theEmitter->emitIns_R_I(INS_bic, EA_8BYTE, REG_V1, 0x2200, INS_OPTS_4H); // LSL  8
    theEmitter->emitIns_R_I(INS_bic, EA_16BYTE, REG_V2, 0x0033, INS_OPTS_8H);
    theEmitter->emitIns_R_I(INS_bic, EA_16BYTE, REG_V3, 0x3300, INS_OPTS_8H); // LSL  8

    theEmitter->emitIns_R_I(INS_bic, EA_8BYTE, REG_V4, 0x42, INS_OPTS_2S);
    theEmitter->emitIns_R_I(INS_bic, EA_8BYTE, REG_V5, 0x4200, INS_OPTS_2S);     // LSL  8
    theEmitter->emitIns_R_I(INS_bic, EA_8BYTE, REG_V6, 0x420000, INS_OPTS_2S);   // LSL 16
    theEmitter->emitIns_R_I(INS_bic, EA_8BYTE, REG_V7, 0x42000000, INS_OPTS_2S); // LSL 24

    theEmitter->emitIns_R_I(INS_bic, EA_16BYTE, REG_V10, 0x5D, INS_OPTS_4S);
    theEmitter->emitIns_R_I(INS_bic, EA_16BYTE, REG_V11, 0x5D00, INS_OPTS_4S);     // LSL  8
    theEmitter->emitIns_R_I(INS_bic, EA_16BYTE, REG_V12, 0x5D0000, INS_OPTS_4S);   // LSL 16
    theEmitter->emitIns_R_I(INS_bic, EA_16BYTE, REG_V13, 0x5D000000, INS_OPTS_4S); // LSL 24

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_F   cmp/fmov immediate
    //

    // fmov  imm8  (scalar)
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V14, 1.0);
    theEmitter->emitIns_R_F(INS_fmov, EA_4BYTE, REG_V15, -1.0);
    theEmitter->emitIns_R_F(INS_fmov, EA_4BYTE, REG_V0, 2.0); // encodes imm8 == 0
    theEmitter->emitIns_R_F(INS_fmov, EA_4BYTE, REG_V16, 10.0);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V17, -10.0);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V18, 31); // Largest encodable value
    theEmitter->emitIns_R_F(INS_fmov, EA_4BYTE, REG_V19, -31);
    theEmitter->emitIns_R_F(INS_fmov, EA_4BYTE, REG_V20, 1.25);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V21, -1.25);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V22, 0.125); // Smallest encodable value
    theEmitter->emitIns_R_F(INS_fmov, EA_4BYTE, REG_V23, -0.125);

    // fmov  imm8  (vector)
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V0, 2.0, INS_OPTS_2S);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V24, 1.0, INS_OPTS_2S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V25, 1.0, INS_OPTS_4S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V26, 1.0, INS_OPTS_2D);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V27, -10.0, INS_OPTS_2S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V28, -10.0, INS_OPTS_4S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V29, -10.0, INS_OPTS_2D);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V30, 31.0, INS_OPTS_2S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V31, 31.0, INS_OPTS_4S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V0, 31.0, INS_OPTS_2D);
    theEmitter->emitIns_R_F(INS_fmov, EA_8BYTE, REG_V1, -0.125, INS_OPTS_2S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V2, -0.125, INS_OPTS_4S);
    theEmitter->emitIns_R_F(INS_fmov, EA_16BYTE, REG_V3, -0.125, INS_OPTS_2D);

    // fcmp with 0.0
    theEmitter->emitIns_R_F(INS_fcmp, EA_8BYTE, REG_V12, 0.0);
    theEmitter->emitIns_R_F(INS_fcmp, EA_4BYTE, REG_V13, 0.0);
    theEmitter->emitIns_R_F(INS_fcmpe, EA_8BYTE, REG_V14, 0.0);
    theEmitter->emitIns_R_F(INS_fcmpe, EA_4BYTE, REG_V15, 0.0);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R   fmov/fcmp/fcvt
    //

    // fmov to vector to vector
    theEmitter->emitIns_R_R(INS_fmov, EA_8BYTE, REG_V0, REG_V2);
    theEmitter->emitIns_R_R(INS_fmov, EA_4BYTE, REG_V1, REG_V3);

    // fmov to vector to general
    theEmitter->emitIns_R_R(INS_fmov, EA_8BYTE, REG_R0, REG_V4);
    theEmitter->emitIns_R_R(INS_fmov, EA_4BYTE, REG_R1, REG_V5);
    //    using the optional conversion specifier
    theEmitter->emitIns_R_R(INS_fmov, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_D_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fmov, EA_4BYTE, REG_R3, REG_V7, INS_OPTS_S_TO_4BYTE);

    // fmov to general to vector
    theEmitter->emitIns_R_R(INS_fmov, EA_8BYTE, REG_V8, REG_R4);
    theEmitter->emitIns_R_R(INS_fmov, EA_4BYTE, REG_V9, REG_R5);
    //   using the optional conversion specifier
    theEmitter->emitIns_R_R(INS_fmov, EA_8BYTE, REG_V10, REG_R6, INS_OPTS_8BYTE_TO_D);
    theEmitter->emitIns_R_R(INS_fmov, EA_4BYTE, REG_V11, REG_R7, INS_OPTS_4BYTE_TO_S);

    // fcmp/fcmpe
    theEmitter->emitIns_R_R(INS_fcmp, EA_8BYTE, REG_V8, REG_V16);
    theEmitter->emitIns_R_R(INS_fcmp, EA_4BYTE, REG_V9, REG_V17);
    theEmitter->emitIns_R_R(INS_fcmpe, EA_8BYTE, REG_V10, REG_V18);
    theEmitter->emitIns_R_R(INS_fcmpe, EA_4BYTE, REG_V11, REG_V19);

    // fcvt
    theEmitter->emitIns_R_R(INS_fcvt, EA_8BYTE, REG_V24, REG_V25, INS_OPTS_S_TO_D); // Single to Double
    theEmitter->emitIns_R_R(INS_fcvt, EA_4BYTE, REG_V26, REG_V27, INS_OPTS_D_TO_S); // Double to Single

    theEmitter->emitIns_R_R(INS_fcvt, EA_4BYTE, REG_V1, REG_V2, INS_OPTS_H_TO_S);
    theEmitter->emitIns_R_R(INS_fcvt, EA_8BYTE, REG_V3, REG_V4, INS_OPTS_H_TO_D);

    theEmitter->emitIns_R_R(INS_fcvt, EA_2BYTE, REG_V5, REG_V6, INS_OPTS_S_TO_H);
    theEmitter->emitIns_R_R(INS_fcvt, EA_2BYTE, REG_V7, REG_V8, INS_OPTS_D_TO_H);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R   floating point conversions
    //

    // fcvtas scalar
    theEmitter->emitIns_R_R(INS_fcvtas, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtas, EA_8BYTE, REG_V2, REG_V3);

    // fcvtas scalar to general
    theEmitter->emitIns_R_R(INS_fcvtas, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtas, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtas, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtas, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtas vector
    theEmitter->emitIns_R_R(INS_fcvtas, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtas, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtas, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    // fcvtau scalar
    theEmitter->emitIns_R_R(INS_fcvtau, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtau, EA_8BYTE, REG_V2, REG_V3);

    // fcvtau scalar to general
    theEmitter->emitIns_R_R(INS_fcvtau, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtau, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtau, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtau, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtau vector
    theEmitter->emitIns_R_R(INS_fcvtau, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtau, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtau, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    ////////////////////////////////////////////////////////////////////////////////

    // fcvtms scalar
    theEmitter->emitIns_R_R(INS_fcvtms, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtms, EA_8BYTE, REG_V2, REG_V3);

    // fcvtms scalar to general
    theEmitter->emitIns_R_R(INS_fcvtms, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtms, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtms, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtms, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtms vector
    theEmitter->emitIns_R_R(INS_fcvtms, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtms, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtms, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    // fcvtmu scalar
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_8BYTE, REG_V2, REG_V3);

    // fcvtmu scalar to general
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtmu vector
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtmu, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    ////////////////////////////////////////////////////////////////////////////////

    // fcvtns scalar
    theEmitter->emitIns_R_R(INS_fcvtns, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtns, EA_8BYTE, REG_V2, REG_V3);

    // fcvtns scalar to general
    theEmitter->emitIns_R_R(INS_fcvtns, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtns, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtns, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtns, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtns vector
    theEmitter->emitIns_R_R(INS_fcvtns, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtns, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtns, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    // fcvtnu scalar
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_8BYTE, REG_V2, REG_V3);

    // fcvtnu scalar to general
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtnu vector
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtnu, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    ////////////////////////////////////////////////////////////////////////////////

    // fcvtps scalar
    theEmitter->emitIns_R_R(INS_fcvtps, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtps, EA_8BYTE, REG_V2, REG_V3);

    // fcvtps scalar to general
    theEmitter->emitIns_R_R(INS_fcvtps, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtps, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtps, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtps, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtps vector
    theEmitter->emitIns_R_R(INS_fcvtps, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtps, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtps, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    // fcvtpu scalar
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_8BYTE, REG_V2, REG_V3);

    // fcvtpu scalar to general
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtpu vector
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtpu, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    ////////////////////////////////////////////////////////////////////////////////

    // fcvtzs scalar
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_8BYTE, REG_V2, REG_V3);

    // fcvtzs scalar to general
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtzs vector
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtzs, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    // fcvtzu scalar
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_8BYTE, REG_V2, REG_V3);

    // fcvtzu scalar to general
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_4BYTE, REG_R0, REG_V4, INS_OPTS_S_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_4BYTE, REG_R1, REG_V5, INS_OPTS_D_TO_4BYTE);
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_8BYTE, REG_R2, REG_V6, INS_OPTS_S_TO_8BYTE);
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_8BYTE, REG_R3, REG_V7, INS_OPTS_D_TO_8BYTE);

    // fcvtzu vector
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fcvtzu, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    ////////////////////////////////////////////////////////////////////////////////

    // scvtf scalar
    theEmitter->emitIns_R_R(INS_scvtf, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_scvtf, EA_8BYTE, REG_V2, REG_V3);

    // scvtf scalar from general
    theEmitter->emitIns_R_R(INS_scvtf, EA_4BYTE, REG_V4, REG_R0, INS_OPTS_4BYTE_TO_S);
    theEmitter->emitIns_R_R(INS_scvtf, EA_4BYTE, REG_V5, REG_R1, INS_OPTS_8BYTE_TO_S);
    theEmitter->emitIns_R_R(INS_scvtf, EA_8BYTE, REG_V6, REG_R2, INS_OPTS_4BYTE_TO_D);
    theEmitter->emitIns_R_R(INS_scvtf, EA_8BYTE, REG_V7, REG_R3, INS_OPTS_8BYTE_TO_D);

    // scvtf vector
    theEmitter->emitIns_R_R(INS_scvtf, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_scvtf, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_scvtf, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

    // ucvtf scalar
    theEmitter->emitIns_R_R(INS_ucvtf, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_ucvtf, EA_8BYTE, REG_V2, REG_V3);

    // ucvtf scalar from general
    theEmitter->emitIns_R_R(INS_ucvtf, EA_4BYTE, REG_V4, REG_R0, INS_OPTS_4BYTE_TO_S);
    theEmitter->emitIns_R_R(INS_ucvtf, EA_4BYTE, REG_V5, REG_R1, INS_OPTS_8BYTE_TO_S);
    theEmitter->emitIns_R_R(INS_ucvtf, EA_8BYTE, REG_V6, REG_R2, INS_OPTS_4BYTE_TO_D);
    theEmitter->emitIns_R_R(INS_ucvtf, EA_8BYTE, REG_V7, REG_R3, INS_OPTS_8BYTE_TO_D);

    // ucvtf vector
    theEmitter->emitIns_R_R(INS_ucvtf, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_ucvtf, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_ucvtf, EA_16BYTE, REG_V12, REG_V13, INS_OPTS_2D);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R   floating point operations, one dest, one source
    //

    // fabs scalar
    theEmitter->emitIns_R_R(INS_fabs, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fabs, EA_8BYTE, REG_V2, REG_V3);

    // fabs vector
    theEmitter->emitIns_R_R(INS_fabs, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fabs, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fabs, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // fneg scalar
    theEmitter->emitIns_R_R(INS_fneg, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fneg, EA_8BYTE, REG_V2, REG_V3);

    // fneg vector
    theEmitter->emitIns_R_R(INS_fneg, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fneg, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fneg, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // fsqrt scalar
    theEmitter->emitIns_R_R(INS_fsqrt, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_fsqrt, EA_8BYTE, REG_V2, REG_V3);

    // fsqrt vector
    theEmitter->emitIns_R_R(INS_fsqrt, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_fsqrt, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_fsqrt, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    genDefineTempLabel(genCreateTempLabel());

    // abs scalar
    theEmitter->emitIns_R_R(INS_abs, EA_8BYTE, REG_V2, REG_V3);

    // abs vector
    theEmitter->emitIns_R_R(INS_abs, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_abs, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_abs, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_abs, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_abs, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_abs, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_abs, EA_16BYTE, REG_V16, REG_V17, INS_OPTS_2D);

    // neg scalar
    theEmitter->emitIns_R_R(INS_neg, EA_8BYTE, REG_V2, REG_V3);

    // neg vector
    theEmitter->emitIns_R_R(INS_neg, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_neg, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_neg, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_neg, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_neg, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_neg, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_neg, EA_16BYTE, REG_V16, REG_V17, INS_OPTS_2D);

    // mvn vector
    theEmitter->emitIns_R_R(INS_mvn, EA_8BYTE, REG_V4, REG_V5);
    theEmitter->emitIns_R_R(INS_mvn, EA_8BYTE, REG_V6, REG_V7, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_mvn, EA_16BYTE, REG_V8, REG_V9);
    theEmitter->emitIns_R_R(INS_mvn, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_16B);

    // cnt vector
    theEmitter->emitIns_R_R(INS_cnt, EA_8BYTE, REG_V22, REG_V23, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_cnt, EA_16BYTE, REG_V24, REG_V25, INS_OPTS_16B);

    // not vector (the same encoding as mvn)
    theEmitter->emitIns_R_R(INS_not, EA_8BYTE, REG_V12, REG_V13);
    theEmitter->emitIns_R_R(INS_not, EA_8BYTE, REG_V14, REG_V15, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_not, EA_16BYTE, REG_V16, REG_V17);
    theEmitter->emitIns_R_R(INS_not, EA_16BYTE, REG_V18, REG_V19, INS_OPTS_16B);

    // cls vector
    theEmitter->emitIns_R_R(INS_cls, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_cls, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_cls, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_cls, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_cls, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_cls, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // clz vector
    theEmitter->emitIns_R_R(INS_clz, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_clz, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_clz, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_clz, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_clz, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_clz, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // rbit vector
    theEmitter->emitIns_R_R(INS_rbit, EA_8BYTE, REG_V0, REG_V1, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_rbit, EA_16BYTE, REG_V2, REG_V3, INS_OPTS_16B);

    // rev16 vector
    theEmitter->emitIns_R_R(INS_rev16, EA_8BYTE, REG_V0, REG_V1, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_rev16, EA_16BYTE, REG_V2, REG_V3, INS_OPTS_16B);

    // rev32 vector
    theEmitter->emitIns_R_R(INS_rev32, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_rev32, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_rev32, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_rev32, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);

    // rev64 vector
    theEmitter->emitIns_R_R(INS_rev64, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_rev64, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_rev64, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_rev64, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_rev64, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_rev64, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // addv vector
    theEmitter->emitIns_R_R(INS_addv, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_addv, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_addv, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_addv, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_addv, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_addv, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // saddlv vector
    theEmitter->emitIns_R_R(INS_saddlv, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_saddlv, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_saddlv, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_saddlv, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_saddlv, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_saddlv, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // smaxlv vector
    theEmitter->emitIns_R_R(INS_smaxlv, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_smaxlv, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_smaxlv, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_smaxlv, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_smaxlv, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_smaxlv, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // sminlv vector
    theEmitter->emitIns_R_R(INS_sminlv, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_sminlv, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_sminlv, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_sminlv, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_sminlv, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_sminlv, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // uaddlv vector
    theEmitter->emitIns_R_R(INS_uaddlv, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_uaddlv, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_uaddlv, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_uaddlv, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_uaddlv, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_uaddlv, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // umaxlv vector
    theEmitter->emitIns_R_R(INS_umaxlv, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_umaxlv, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_umaxlv, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_umaxlv, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_umaxlv, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_umaxlv, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // uminlv vector
    theEmitter->emitIns_R_R(INS_uminlv, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_uminlv, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_uminlv, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_uminlv, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_uminlv, EA_8BYTE, REG_V12, REG_V13, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_uminlv, EA_16BYTE, REG_V14, REG_V15, INS_OPTS_4S);

    // faddp scalar
    theEmitter->emitIns_R_R(INS_faddp, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_faddp, EA_8BYTE, REG_V2, REG_V3);

    // INS_fcvtl
    theEmitter->emitIns_R_R(INS_fcvtl, EA_4BYTE, REG_V0, REG_V1);

    // INS_fcvtl2
    theEmitter->emitIns_R_R(INS_fcvtl2, EA_4BYTE, REG_V0, REG_V1);

    // INS_fcvtn
    theEmitter->emitIns_R_R(INS_fcvtn, EA_8BYTE, REG_V0, REG_V1);

    // INS_fcvtn2
    theEmitter->emitIns_R_R(INS_fcvtn2, EA_8BYTE, REG_V0, REG_V1);
#endif

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R   floating point round to int, one dest, one source
    //

    // frinta scalar
    theEmitter->emitIns_R_R(INS_frinta, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_frinta, EA_8BYTE, REG_V2, REG_V3);

    // frinta vector
    theEmitter->emitIns_R_R(INS_frinta, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_frinta, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_frinta, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // frinti scalar
    theEmitter->emitIns_R_R(INS_frinti, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_frinti, EA_8BYTE, REG_V2, REG_V3);

    // frinti vector
    theEmitter->emitIns_R_R(INS_frinti, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_frinti, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_frinti, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // frintm scalar
    theEmitter->emitIns_R_R(INS_frintm, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_frintm, EA_8BYTE, REG_V2, REG_V3);

    // frintm vector
    theEmitter->emitIns_R_R(INS_frintm, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_frintm, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_frintm, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // frintn scalar
    theEmitter->emitIns_R_R(INS_frintn, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_frintn, EA_8BYTE, REG_V2, REG_V3);

    // frintn vector
    theEmitter->emitIns_R_R(INS_frintn, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_frintn, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_frintn, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // frintp scalar
    theEmitter->emitIns_R_R(INS_frintp, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_frintp, EA_8BYTE, REG_V2, REG_V3);

    // frintp vector
    theEmitter->emitIns_R_R(INS_frintp, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_frintp, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_frintp, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // frintx scalar
    theEmitter->emitIns_R_R(INS_frintx, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_frintx, EA_8BYTE, REG_V2, REG_V3);

    // frintx vector
    theEmitter->emitIns_R_R(INS_frintx, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_frintx, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_frintx, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

    // frintz scalar
    theEmitter->emitIns_R_R(INS_frintz, EA_4BYTE, REG_V0, REG_V1);
    theEmitter->emitIns_R_R(INS_frintz, EA_8BYTE, REG_V2, REG_V3);

    // frintz vector
    theEmitter->emitIns_R_R(INS_frintz, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_frintz, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_4S);
    theEmitter->emitIns_R_R(INS_frintz, EA_16BYTE, REG_V8, REG_V9, INS_OPTS_2D);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R   floating point operations, one dest, two source
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R(INS_fadd, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fadd, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fadd, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fadd, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fadd, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_fsub, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fsub, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fsub, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fsub, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fsub, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_fdiv, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fdiv, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fdiv, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fdiv, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fdiv, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_fmax, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fmax, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fmax, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fmax, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fmax, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_fmin, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fmin, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fmin, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fmin, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fmin, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    // fabd
    theEmitter->emitIns_R_R_R(INS_fabd, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fabd, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fabd, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fabd, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fabd, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R(INS_fmul, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fmul, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fmul, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fmul, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fmul, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R_I(INS_fmul, EA_4BYTE, REG_V15, REG_V16, REG_V17, 3); // scalar by elem 4BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmul, EA_8BYTE, REG_V18, REG_V19, REG_V20, 1); // scalar by elem 8BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmul, EA_8BYTE, REG_V21, REG_V22, REG_V23, 0, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_fmul, EA_16BYTE, REG_V24, REG_V25, REG_V26, 2, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_fmul, EA_16BYTE, REG_V27, REG_V28, REG_V29, 0, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_fmulx, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fmulx, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_fmulx, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fmulx, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fmulx, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R_I(INS_fmulx, EA_4BYTE, REG_V15, REG_V16, REG_V17, 3); // scalar by elem 4BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmulx, EA_8BYTE, REG_V18, REG_V19, REG_V20, 1); // scalar by elem 8BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmulx, EA_8BYTE, REG_V21, REG_V22, REG_V23, 0, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_fmulx, EA_16BYTE, REG_V24, REG_V25, REG_V26, 2, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_fmulx, EA_16BYTE, REG_V27, REG_V28, REG_V29, 0, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_fnmul, EA_4BYTE, REG_V0, REG_V1, REG_V2); // scalar 4BYTE
    theEmitter->emitIns_R_R_R(INS_fnmul, EA_8BYTE, REG_V3, REG_V4, REG_V5); // scalar 8BYTE

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_I  vector operations, one dest, one source reg, one immed
    //

    genDefineTempLabel(genCreateTempLabel());

    // 'sshr' scalar
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'sshr' vector
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_sshr, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'ssra' scalar
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'ssra' vector
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_ssra, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'srshr' scalar
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'srshr' vector
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_srshr, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'srsra' scalar
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'srsra' vector
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_srsra, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'shl' scalar
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'shl' vector
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_shl, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_shl, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_shl, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_shl, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_shl, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_shl, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'ushr' scalar
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'ushr' vector
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_ushr, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'usra' scalar
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'usra' vector
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_usra, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_usra, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_usra, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_usra, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_usra, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_usra, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'urshr' scalar
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'urshr' vector
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_urshr, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'ursra' scalar
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'srsra' vector
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_ursra, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'sri' scalar
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'sri' vector
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_sri, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_sri, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_sri, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_sri, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_sri, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_sri, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'sli' scalar
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V0, REG_V1, 1);
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V2, REG_V3, 14);
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V4, REG_V5, 27);
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V6, REG_V7, 40);
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V8, REG_V9, 63);

    // 'sli' vector
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_sli, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_sli, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_sli, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_sli, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);
    theEmitter->emitIns_R_R_I(INS_sli, EA_16BYTE, REG_V12, REG_V13, 33, INS_OPTS_2D);
    theEmitter->emitIns_R_R_I(INS_sli, EA_16BYTE, REG_V14, REG_V15, 63, INS_OPTS_2D);

    // 'sshll' vector
    theEmitter->emitIns_R_R_I(INS_sshll, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_sshll2, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_sshll, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_sshll2, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_sshll, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_sshll2, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);

    // 'ushll' vector
    theEmitter->emitIns_R_R_I(INS_ushll, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_ushll2, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_ushll, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_ushll2, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_ushll, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_ushll2, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);

    // 'shrn' vector
    theEmitter->emitIns_R_R_I(INS_shrn, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_shrn2, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_shrn, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_shrn2, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_shrn, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_shrn2, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);

    // 'rshrn' vector
    theEmitter->emitIns_R_R_I(INS_rshrn, EA_8BYTE, REG_V0, REG_V1, 1, INS_OPTS_8B);
    theEmitter->emitIns_R_R_I(INS_rshrn2, EA_16BYTE, REG_V2, REG_V3, 7, INS_OPTS_16B);
    theEmitter->emitIns_R_R_I(INS_rshrn, EA_8BYTE, REG_V4, REG_V5, 9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_I(INS_rshrn2, EA_16BYTE, REG_V6, REG_V7, 15, INS_OPTS_8H);
    theEmitter->emitIns_R_R_I(INS_rshrn, EA_8BYTE, REG_V8, REG_V9, 17, INS_OPTS_2S);
    theEmitter->emitIns_R_R_I(INS_rshrn2, EA_16BYTE, REG_V10, REG_V11, 31, INS_OPTS_4S);

    // 'sxtl' vector
    theEmitter->emitIns_R_R(INS_sxtl, EA_8BYTE, REG_V0, REG_V1, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_sxtl2, EA_16BYTE, REG_V2, REG_V3, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_sxtl, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_sxtl2, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_sxtl, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_sxtl2, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);

    // 'uxtl' vector
    theEmitter->emitIns_R_R(INS_uxtl, EA_8BYTE, REG_V0, REG_V1, INS_OPTS_8B);
    theEmitter->emitIns_R_R(INS_uxtl2, EA_16BYTE, REG_V2, REG_V3, INS_OPTS_16B);
    theEmitter->emitIns_R_R(INS_uxtl, EA_8BYTE, REG_V4, REG_V5, INS_OPTS_4H);
    theEmitter->emitIns_R_R(INS_uxtl2, EA_16BYTE, REG_V6, REG_V7, INS_OPTS_8H);
    theEmitter->emitIns_R_R(INS_uxtl, EA_8BYTE, REG_V8, REG_V9, INS_OPTS_2S);
    theEmitter->emitIns_R_R(INS_uxtl2, EA_16BYTE, REG_V10, REG_V11, INS_OPTS_4S);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R   vector operations, one dest, two source
    //

    genDefineTempLabel(genCreateTempLabel());

    // Specifying an Arrangement is optional
    //
    theEmitter->emitIns_R_R_R(INS_and, EA_8BYTE, REG_V6, REG_V7, REG_V8);
    theEmitter->emitIns_R_R_R(INS_bic, EA_8BYTE, REG_V9, REG_V10, REG_V11);
    theEmitter->emitIns_R_R_R(INS_eor, EA_8BYTE, REG_V12, REG_V13, REG_V14);
    theEmitter->emitIns_R_R_R(INS_orr, EA_8BYTE, REG_V15, REG_V16, REG_V17);
    theEmitter->emitIns_R_R_R(INS_orn, EA_8BYTE, REG_V18, REG_V19, REG_V20);
    theEmitter->emitIns_R_R_R(INS_and, EA_16BYTE, REG_V21, REG_V22, REG_V23);
    theEmitter->emitIns_R_R_R(INS_bic, EA_16BYTE, REG_V24, REG_V25, REG_V26);
    theEmitter->emitIns_R_R_R(INS_eor, EA_16BYTE, REG_V27, REG_V28, REG_V29);
    theEmitter->emitIns_R_R_R(INS_orr, EA_16BYTE, REG_V30, REG_V31, REG_V0);
    theEmitter->emitIns_R_R_R(INS_orn, EA_16BYTE, REG_V1, REG_V2, REG_V3);

    theEmitter->emitIns_R_R_R(INS_bsl, EA_8BYTE, REG_V4, REG_V5, REG_V6);
    theEmitter->emitIns_R_R_R(INS_bit, EA_8BYTE, REG_V7, REG_V8, REG_V9);
    theEmitter->emitIns_R_R_R(INS_bif, EA_8BYTE, REG_V10, REG_V11, REG_V12);
    theEmitter->emitIns_R_R_R(INS_bsl, EA_16BYTE, REG_V13, REG_V14, REG_V15);
    theEmitter->emitIns_R_R_R(INS_bit, EA_16BYTE, REG_V16, REG_V17, REG_V18);
    theEmitter->emitIns_R_R_R(INS_bif, EA_16BYTE, REG_V19, REG_V20, REG_V21);

    // Default Arrangement as per the ARM64 manual
    //
    theEmitter->emitIns_R_R_R(INS_and, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_bic, EA_8BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_eor, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_orr, EA_8BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_orn, EA_8BYTE, REG_V18, REG_V19, REG_V20, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_and, EA_16BYTE, REG_V21, REG_V22, REG_V23, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_bic, EA_16BYTE, REG_V24, REG_V25, REG_V26, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_eor, EA_16BYTE, REG_V27, REG_V28, REG_V29, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_orr, EA_16BYTE, REG_V30, REG_V31, REG_V0, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_orn, EA_16BYTE, REG_V1, REG_V2, REG_V3, INS_OPTS_16B);

    theEmitter->emitIns_R_R_R(INS_bsl, EA_8BYTE, REG_V4, REG_V5, REG_V6, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_bit, EA_8BYTE, REG_V7, REG_V8, REG_V9, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_bif, EA_8BYTE, REG_V10, REG_V11, REG_V12, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_bsl, EA_16BYTE, REG_V13, REG_V14, REG_V15, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_bit, EA_16BYTE, REG_V16, REG_V17, REG_V18, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_bif, EA_16BYTE, REG_V19, REG_V20, REG_V21, INS_OPTS_16B);

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R(INS_add, EA_8BYTE, REG_V0, REG_V1, REG_V2); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_add, EA_8BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_add, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_add, EA_8BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_add, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_add, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_add, EA_16BYTE, REG_V18, REG_V19, REG_V20, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_add, EA_16BYTE, REG_V21, REG_V22, REG_V23, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_sub, EA_8BYTE, REG_V1, REG_V2, REG_V3); // scalar 8BYTE
    theEmitter->emitIns_R_R_R(INS_sub, EA_8BYTE, REG_V4, REG_V5, REG_V6, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_sub, EA_8BYTE, REG_V7, REG_V8, REG_V9, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_sub, EA_8BYTE, REG_V10, REG_V11, REG_V12, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_sub, EA_16BYTE, REG_V13, REG_V14, REG_V15, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_sub, EA_16BYTE, REG_V16, REG_V17, REG_V18, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_sub, EA_16BYTE, REG_V19, REG_V20, REG_V21, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_sub, EA_16BYTE, REG_V22, REG_V23, REG_V24, INS_OPTS_2D);

    genDefineTempLabel(genCreateTempLabel());

    // saba vector
    theEmitter->emitIns_R_R_R(INS_saba, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_saba, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_saba, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_saba, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_saba, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_saba, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    // sabd vector
    theEmitter->emitIns_R_R_R(INS_sabd, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_sabd, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_sabd, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_sabd, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_sabd, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_sabd, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    // uaba vector
    theEmitter->emitIns_R_R_R(INS_uaba, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_uaba, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_uaba, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_uaba, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_uaba, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_uaba, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    // uabd vector
    theEmitter->emitIns_R_R_R(INS_uabd, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_uabd, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_uabd, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_uabd, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_uabd, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_uabd, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    // smax vector
    theEmitter->emitIns_R_R_R(INS_smax, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_smax, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_smax, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_smax, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_smax, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_smax, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    // smin vector
    theEmitter->emitIns_R_R_R(INS_smin, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_smin, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_smin, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_smin, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_smin, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_smin, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    // umax vector
    theEmitter->emitIns_R_R_R(INS_umax, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_umax, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_umax, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_umax, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_umax, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_umax, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    // umin vector
    theEmitter->emitIns_R_R_R(INS_umin, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_umin, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_umin, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_umin, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_umin, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_umin, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    // cmeq vector
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_1D);
    theEmitter->emitIns_R_R_R(INS_cmeq, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // cmge vector
    theEmitter->emitIns_R_R_R(INS_cmge, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_cmge, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_cmge, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_cmge, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_cmge, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_cmge, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_cmge, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_1D);
    theEmitter->emitIns_R_R_R(INS_cmge, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // cmgt vector
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_1D);
    theEmitter->emitIns_R_R_R(INS_cmgt, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // cmhi vector
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_1D);
    theEmitter->emitIns_R_R_R(INS_cmhi, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // cmhs vector
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_1D);
    theEmitter->emitIns_R_R_R(INS_cmhs, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // ctst vector
    theEmitter->emitIns_R_R_R(INS_ctst, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_ctst, EA_16BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_ctst, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_ctst, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_ctst, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_ctst, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_ctst, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_1D);
    theEmitter->emitIns_R_R_R(INS_ctst, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // faddp vector
    theEmitter->emitIns_R_R_R(INS_faddp, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_faddp, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_faddp, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // fcmeq vector
    theEmitter->emitIns_R_R_R(INS_fcmeq, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fcmeq, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fcmeq, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // fcmge vector
    theEmitter->emitIns_R_R_R(INS_fcmge, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fcmge, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fcmge, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);

    // fcmgt vector
    theEmitter->emitIns_R_R_R(INS_fcmgt, EA_8BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fcmgt, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fcmgt, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_2D);
#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R  vector multiply
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R(INS_mul, EA_8BYTE, REG_V0, REG_V1, REG_V2, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_mul, EA_8BYTE, REG_V3, REG_V4, REG_V5, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R(INS_mul, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_mul, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_16B);
    theEmitter->emitIns_R_R_R(INS_mul, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R(INS_mul, EA_16BYTE, REG_V15, REG_V16, REG_V17, INS_OPTS_4S);

    theEmitter->emitIns_R_R_R(INS_pmul, EA_8BYTE, REG_V18, REG_V19, REG_V20, INS_OPTS_8B);
    theEmitter->emitIns_R_R_R(INS_pmul, EA_16BYTE, REG_V21, REG_V22, REG_V23, INS_OPTS_16B);

    // 'mul' vector by elem
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_8BYTE, REG_V0, REG_V1, REG_V16, 0, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_8BYTE, REG_V2, REG_V3, REG_V15, 1, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_8BYTE, REG_V4, REG_V5, REG_V17, 3, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_8BYTE, REG_V6, REG_V7, REG_V0, 0, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_8BYTE, REG_V8, REG_V9, REG_V1, 3, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_8BYTE, REG_V10, REG_V11, REG_V2, 7, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_16BYTE, REG_V12, REG_V13, REG_V14, 0, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_16BYTE, REG_V14, REG_V15, REG_V18, 1, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_16BYTE, REG_V16, REG_V17, REG_V13, 3, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_16BYTE, REG_V18, REG_V19, REG_V3, 0, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_16BYTE, REG_V20, REG_V21, REG_V4, 3, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R_I(INS_mul, EA_16BYTE, REG_V22, REG_V23, REG_V5, 7, INS_OPTS_8H);

    // 'mla' vector by elem
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_8BYTE, REG_V0, REG_V1, REG_V16, 0, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_8BYTE, REG_V2, REG_V3, REG_V15, 1, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_8BYTE, REG_V4, REG_V5, REG_V17, 3, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_8BYTE, REG_V6, REG_V7, REG_V0, 0, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_8BYTE, REG_V8, REG_V9, REG_V1, 3, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_8BYTE, REG_V10, REG_V11, REG_V2, 7, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_16BYTE, REG_V12, REG_V13, REG_V14, 0, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_16BYTE, REG_V14, REG_V15, REG_V18, 1, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_16BYTE, REG_V16, REG_V17, REG_V13, 3, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_16BYTE, REG_V18, REG_V19, REG_V3, 0, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_16BYTE, REG_V20, REG_V21, REG_V4, 3, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R_I(INS_mla, EA_16BYTE, REG_V22, REG_V23, REG_V5, 7, INS_OPTS_8H);

    // 'mls' vector by elem
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_8BYTE, REG_V0, REG_V1, REG_V16, 0, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_8BYTE, REG_V2, REG_V3, REG_V15, 1, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_8BYTE, REG_V4, REG_V5, REG_V17, 3, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_8BYTE, REG_V6, REG_V7, REG_V0, 0, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_8BYTE, REG_V8, REG_V9, REG_V1, 3, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_8BYTE, REG_V10, REG_V11, REG_V2, 7, INS_OPTS_4H);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_16BYTE, REG_V12, REG_V13, REG_V14, 0, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_16BYTE, REG_V14, REG_V15, REG_V18, 1, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_16BYTE, REG_V16, REG_V17, REG_V13, 3, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_16BYTE, REG_V18, REG_V19, REG_V3, 0, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_16BYTE, REG_V20, REG_V21, REG_V4, 3, INS_OPTS_8H);
    theEmitter->emitIns_R_R_R_I(INS_mls, EA_16BYTE, REG_V22, REG_V23, REG_V5, 7, INS_OPTS_8H);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R   floating point operations, one source/dest, and two source
    //

    genDefineTempLabel(genCreateTempLabel());

    theEmitter->emitIns_R_R_R(INS_fmla, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fmla, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fmla, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R_I(INS_fmla, EA_4BYTE, REG_V15, REG_V16, REG_V17, 3); // scalar by elem 4BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmla, EA_8BYTE, REG_V18, REG_V19, REG_V20, 1); // scalar by elem 8BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmla, EA_8BYTE, REG_V21, REG_V22, REG_V23, 0, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_fmla, EA_16BYTE, REG_V24, REG_V25, REG_V26, 2, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_fmla, EA_16BYTE, REG_V27, REG_V28, REG_V29, 0, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R(INS_fmls, EA_8BYTE, REG_V6, REG_V7, REG_V8, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R(INS_fmls, EA_16BYTE, REG_V9, REG_V10, REG_V11, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R(INS_fmls, EA_16BYTE, REG_V12, REG_V13, REG_V14, INS_OPTS_2D);

    theEmitter->emitIns_R_R_R_I(INS_fmls, EA_4BYTE, REG_V15, REG_V16, REG_V17, 3); // scalar by elem 4BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmls, EA_8BYTE, REG_V18, REG_V19, REG_V20, 1); // scalar by elem 8BYTE
    theEmitter->emitIns_R_R_R_I(INS_fmls, EA_8BYTE, REG_V21, REG_V22, REG_V23, 0, INS_OPTS_2S);
    theEmitter->emitIns_R_R_R_I(INS_fmls, EA_16BYTE, REG_V24, REG_V25, REG_V26, 2, INS_OPTS_4S);
    theEmitter->emitIns_R_R_R_I(INS_fmls, EA_16BYTE, REG_V27, REG_V28, REG_V29, 0, INS_OPTS_2D);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS
    //
    // R_R_R_R   floating point operations, one dest, and three source
    //

    theEmitter->emitIns_R_R_R_R(INS_fmadd, EA_4BYTE, REG_V0, REG_V8, REG_V16, REG_V24);
    theEmitter->emitIns_R_R_R_R(INS_fmsub, EA_4BYTE, REG_V1, REG_V9, REG_V17, REG_V25);
    theEmitter->emitIns_R_R_R_R(INS_fnmadd, EA_4BYTE, REG_V2, REG_V10, REG_V18, REG_V26);
    theEmitter->emitIns_R_R_R_R(INS_fnmsub, EA_4BYTE, REG_V3, REG_V11, REG_V19, REG_V27);

    theEmitter->emitIns_R_R_R_R(INS_fmadd, EA_8BYTE, REG_V4, REG_V12, REG_V20, REG_V28);
    theEmitter->emitIns_R_R_R_R(INS_fmsub, EA_8BYTE, REG_V5, REG_V13, REG_V21, REG_V29);
    theEmitter->emitIns_R_R_R_R(INS_fnmadd, EA_8BYTE, REG_V6, REG_V14, REG_V22, REG_V30);
    theEmitter->emitIns_R_R_R_R(INS_fnmsub, EA_8BYTE, REG_V7, REG_V15, REG_V23, REG_V31);

#endif

#ifdef ALL_ARM64_EMITTER_UNIT_TESTS

    BasicBlock* label = genCreateTempLabel();
    genDefineTempLabel(label);
    instGen(INS_nop);
    instGen(INS_nop);
    instGen(INS_nop);
    instGen(INS_nop);
    theEmitter->emitIns_R_L(INS_adr, EA_4BYTE_DSP_RELOC, label, REG_R0);

#endif // ALL_ARM64_EMITTER_UNIT_TESTS

    printf("*************** End of genArm64EmitterUnitTests()\n");
}
#endif // defined(DEBUG)

#endif // _TARGET_ARM64_
