// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <dynarmic/dynarmic.h>
#include "common/assert.h"
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/svc.h"
#include "core/memory.h"

static void InterpreterFallback(u32 pc, Dynarmic::Jit* jit, void* user_arg) {
    ARMul_State* state = static_cast<ARMul_State*>(user_arg);

    state->Reg = jit->Regs();
    state->Cpsr = jit->Cpsr();
    state->Reg[15] = pc;
    state->ExtReg = jit->ExtRegs();
    state->VFP[VFP_FPSCR] = jit->Fpscr();
    state->NumInstrsToExecute = 1;

    InterpreterMainLoop(state);

    bool is_thumb = (state->Cpsr & (1 << 5)) != 0;
    state->Reg[15] &= (is_thumb ? 0xFFFFFFFE : 0xFFFFFFFC);

    jit->Regs() = state->Reg;
    jit->Cpsr() = state->Cpsr;
    jit->ExtRegs() = state->ExtReg;
    jit->SetFpscr(state->VFP[VFP_FPSCR]);
}

static bool IsReadOnlyMemory(u32 vaddr) {
    // TODO(bunnei): ImplementMe
    return false;
}

static Dynarmic::UserCallbacks GetUserCallbacks(
    const std::shared_ptr<ARMul_State>& interpeter_state, Memory::PageTable* current_page_table) {
    Dynarmic::UserCallbacks user_callbacks{};
    user_callbacks.InterpreterFallback = &InterpreterFallback;
    user_callbacks.user_arg = static_cast<void*>(interpeter_state.get());
    user_callbacks.CallSVC = &SVC::CallSVC;
    user_callbacks.memory.IsReadOnlyMemory = &IsReadOnlyMemory;
    user_callbacks.memory.ReadCode = &Memory::Read32;
    user_callbacks.memory.Read8 = &Memory::Read8;
    user_callbacks.memory.Read16 = &Memory::Read16;
    user_callbacks.memory.Read32 = &Memory::Read32;
    user_callbacks.memory.Read64 = &Memory::Read64;
    user_callbacks.memory.Write8 = &Memory::Write8;
    user_callbacks.memory.Write16 = &Memory::Write16;
    user_callbacks.memory.Write32 = &Memory::Write32;
    user_callbacks.memory.Write64 = &Memory::Write64;
    user_callbacks.page_table = &current_page_table->pointers;
    user_callbacks.coprocessors[15] = std::make_shared<DynarmicCP15>(interpeter_state);
    return user_callbacks;
}

ARM_Dynarmic::ARM_Dynarmic(PrivilegeMode initial_mode) {
    interpreter_state = std::make_shared<ARMul_State>(initial_mode);
    PageTableChanged();
}

void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    if (reg == VFP_FPSCR) {
        return jit->Fpscr();
    }

    // Dynarmic does not implement and/or expose other VFP registers, fallback to interpreter state
    return interpreter_state->VFP[reg];
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    if (reg == VFP_FPSCR) {
        jit->SetFpscr(value);
    }

    // Dynarmic does not implement and/or expose other VFP registers, fallback to interpreter state
    interpreter_state->VFP[reg] = value;
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->Cpsr() = cpsr;
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) {
    return interpreter_state->CP15[reg];
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    interpreter_state->CP15[reg] = value;
}

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::ExecuteInstructions(int num_instructions) {
    ASSERT(Memory::GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);

    std::size_t ticks_executed = jit->Run(static_cast<unsigned>(num_instructions));

    CoreTiming::AddTicks(4000);
}

void ARM_Dynarmic::SaveContext(ARM_Interface::ThreadContext& ctx) {
    memcpy(ctx.cpu_registers, jit->Regs().data(), sizeof(ctx.cpu_registers));
    memcpy(ctx.fpu_registers, jit->ExtRegs().data(), sizeof(ctx.fpu_registers));

    ctx.sp = jit->Regs()[13];
    ctx.lr = jit->Regs()[14];
    ctx.pc = jit->Regs()[15];
    ctx.cpsr = jit->Cpsr();

    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = interpreter_state->VFP[VFP_FPEXC];
}

void ARM_Dynarmic::LoadContext(const ARM_Interface::ThreadContext& ctx) {
    memcpy(jit->Regs().data(), ctx.cpu_registers, sizeof(ctx.cpu_registers));
    memcpy(jit->ExtRegs().data(), ctx.fpu_registers, sizeof(ctx.fpu_registers));

    jit->Regs()[13] = ctx.sp;
    jit->Regs()[14] = ctx.lr;
    jit->Regs()[15] = ctx.pc;
    jit->Cpsr() = ctx.cpsr;

    jit->SetFpscr(ctx.fpscr);
    interpreter_state->VFP[VFP_FPEXC] = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
    jit->ClearCache();
}

void ARM_Dynarmic::PageTableChanged() {
    current_page_table = Memory::GetCurrentPageTable();

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        return;
    }

    jit = new Dynarmic::Jit(GetUserCallbacks(interpreter_state, current_page_table));
    jits.emplace(current_page_table, std::unique_ptr<Dynarmic::Jit>(jit));
}
