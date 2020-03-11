//
// Created by 甘尧 on 2020-02-29.
//

#pragma once

#include <aarch64/macro-assembler-aarch64.h>
#include "asm/arm64/cpu_arm64.h"
#include <base/marcos.h>
#include "block/code_find_table.h"
#include "dbi_mmu_arm64.h"
#include "block/host_code_block.h"

using namespace vixl::aarch64;
using namespace Code;
using namespace Code::A64;


// TODO Code Invalid 粒度在于一个个 Block，那当某些线程仍跑在旧 Block 中时，新 Block 的改动被提交了，那么该线程仍会跑一段旧代码，syscall 后的代码较易复现
/**
 * 此种 Case 的解决办法在于将该 Block 的旧 Code Cache 中非生成部分置为非法指令
 */

namespace DBI::A64 {

#define __ masm_.
#define CTX_TLS_SLOT 7
#define TMP0 x17
#define TMP1 x16
#define HOST_TLS ({ void** __val; __asm__("mrs %0, tpidr_el0" : "=r"(__val)); __val; })
#define HOST_STACK_SIZE 1U << 20

    class LabelHolder : public BaseObject {
    public:

        LabelHolder(MacroAssembler &masm);
        virtual ~LabelHolder();

        void SetDestBuffer(VAddr addr);

        Label *AllocLabel();
        Label *GetDispatcherLabel();
        Label *GetPageLookupLabel();
        Label *GetCallSvcLabel();
        Label *GetSpecLabel();
        void BindDispatcherTrampoline(VAddr addr);
        void BindPageLookupTrampoline(VAddr addr);
        void BindCallSvcTrampoline(VAddr addr);
        void BindSpecTrampoline(VAddr addr);
    private:
        VAddr dest_buffer_start_;
        MacroAssembler &masm_;
        std::list<Label*> labels_;
        Label dispatcher_label_;
        Label page_lookup_label_;
        Label call_svc_label_;
        Label spec_label_;
    };

    class Context : public BaseObject {
    public:

        Context(const Register &reg_ctx, const Register &reg_forward);
        virtual ~Context();

        static SharedPtr<Context> &Current();

        MacroAssembler &Assembler();

        void SetCodeCachePosition(VAddr addr);
        void FlushCodeCache(CodeBlockRef block, bool bind_stub = true);

        void Emit(u32 instr);

        Label *Label();

        template<u8 temp_count>
        void WrapContext(std::function<void(std::array<Register, temp_count>)> wrap,
                         std::initializer_list<Register> effect_regs = {}) {
            auto temp_regs = GetTmpRegisters<temp_count>(effect_regs);
            LoadContext();
            PushX<temp_count>(temp_regs);
            wrap(temp_regs);
            PopX<temp_count>(temp_regs);
            ClearContext();
        }

        template<u8 temp_count>
        std::array<Register, temp_count>
        GetTmpRegisters(std::initializer_list<Register> effect_regs = {}) {
            std::array<Register, temp_count> temps;
            int index = 0;
            for (int i = 0; i < 29; ++i) {
                if (index == temp_count - 1)
                    break;
                if (i == reg_ctx_.GetCode())
                    continue;
                bool need_skip{false};
                for (auto effect: effect_regs) {
                    if (effect.GetCode() == i) {
                        need_skip = true;
                        break;
                    }
                }
                if (need_skip)
                    continue;
                temps[index] = XRegister::GetXRegFromCode(i);
                index++;
            }
            return temps;
        }

        virtual void LoadContext() {};

        virtual void ClearContext() {};

        template<u8 size>
        void PushX(std::array<Register, size> &xregs) {
            int index = 0;
            while (index < size) {
                // could stp
                if ((index + 1 < size) &&
                    (xregs[index + 1].GetCode() - xregs[index].GetCode() == 1) &&
                    (xregs[index].GetCode() % 2 == 0)) {
                    __ Stp(xregs[index], xregs[index + 1],
                           MemOperand(reg_ctx_, OFFSET_CTX_A64_CPU_REG + xregs[index].GetCode()));
                    index += 2;
                } else {
                    __ Str(xregs[index],
                           MemOperand(reg_ctx_, OFFSET_CTX_A64_CPU_REG + xregs[index].GetCode()));
                    index += 1;
                }
            }
        }

        template<u8 size>
        void PopX(std::array<Register, size> &xregs) {
            int index = 0;
            while (index < size) {
                // could stp
                if ((index + 1 < size) &&
                    (xregs[index + 1].GetCode() - xregs[index].GetCode() == 1) &&
                    (xregs[index].GetCode() % 2 == 0)) {
                    __ Ldp(xregs[index], xregs[index + 1],
                           MemOperand(reg_ctx_, OFFSET_CTX_A64_CPU_REG + xregs[index].GetCode()));
                    index += 2;
                } else {
                    __ Ldr(xregs[index],
                           MemOperand(reg_ctx_, OFFSET_CTX_A64_CPU_REG + xregs[index].GetCode()));
                    index += 1;
                }
            }
        }

        void PushX(Register reg1, Register reg2 = NoReg);

        void PopX(Register reg1, Register reg2 = NoReg);

        void CheckSuspend(Register tmp);

        void SavePc(VAddr pc, Register tmp);

        const CPU::A64::CPUContext &GetCPUContext() const;

        void LoadFromContext(Register target, VAddr offset);

        VAddr GetCurPc() const;

        void SetCurPc(VAddr cur_pc);

        void SetSuspendFlag(bool suspend);

        virtual void GetSp(u8 target) {};

        virtual void GetPc(u8 target) {};

        // sysreg
        void ReadTPIDR(u8 target);
        void ReadTPIDRRO(u8 target);

        // context switch
        virtual void SaveContextFull(bool protect_lr = false);
        virtual void RestoreContextFull(bool protect_lr = false);
        virtual void SaveContextCallerSaved(bool protect_lr = false);
        virtual void RestoreContextCallerSaved(bool protect_lr = false);
        virtual void PrepareHostStack();
        virtual void PrepareGuestStack();

        // brunch
        void FindForwardTarget(u8 reg_target);
        void FindForwardTarget(VAddr const_target);
        void RestoreForwardRegister();
        virtual u32 CodeSizeOfForwardRestore() = 0;

        // system
        void CallSvc(u32 svc_num);

        // trampolines
        void DispatherStub(CodeBlockRef block);
        void CallSvcStub(CodeBlockRef block);
        void SpecStub(CodeBlockRef block);

    protected:
        CPU::A64::CPUContext context_;
        VAddr cur_pc_;
        VAddr host_stack_;
        const Register &reg_ctx_;
        const Register &reg_forward_;
        struct {
            VAddr origin_code_start_;
            SharedPtr<LabelHolder> label_holder_;
        } cursor_;
        // must pic code
        MacroAssembler masm_{PositionIndependentCode};
        u64 suspend_addr_;
        SharedPtr<FindTable<VAddr>> code_find_table_;
    };

    class ContextNoMemTrace : public Context {
    public:
        void LoadContext() override;

        void ClearContext() override;

        u32 CodeSizeOfForwardRestore() override;

    protected:

        void PreDispatch();

        void PostDispatch();

    public:
        ContextNoMemTrace();

        void GetSp(u8 target) override;

        void GetPc(u8 target) override;
    };

    class ContextWithMemTrace : public Context {
    public:
        ContextWithMemTrace(SharedPtr<PageTable> page_table);

        void LookupTLB(u8 reg_addr);

        void LookupFlatPageTable(u8 addr_reg);

        void LookupFlatPageTable(VAddr const_addr, u8 reg);

        void PageLookupStub(CodeBlockRef block);

        void CheckReadSpec(Register pte_reg, Register offset_reg);

        void CheckWriteSpec(Register pte_reg, Register offset_reg);

        u32 CodeSizeOfForwardRestore() override;

    protected:
        u8 address_bits_unused_;
        u8 page_bits_;
        u8 tlb_bits_;
        bool hook_read_spec_;
        bool hook_write_spec_;
        SharedPtr<PageTable> page_table_;
    };

}
