//===-- SBFrame.cpp ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBFrame.h"

#include <string>
#include <algorithm>

#include "lldb/lldb-types.h"

#include "lldb/Core/Address.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Stream.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/ValueObjectRegister.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Expression/ClangUserExpression.h"
#include "lldb/Host/Host.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/StackID.h"
#include "lldb/Target/Thread.h"

#include "lldb/API/SBDebugger.h"
#include "lldb/API/SBValue.h"
#include "lldb/API/SBAddress.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBSymbolContext.h"
#include "lldb/API/SBThread.h"

namespace lldb_private {

    class StackFrameImpl
    {
    public:
        StackFrameImpl (const lldb::StackFrameSP &frame_sp) :
            m_frame_wp (frame_sp),
            m_thread_wp (),
            m_stack_id ()
        {
            if (frame_sp)
            {
                m_thread_wp = frame_sp->GetThread();
                m_stack_id = frame_sp->GetStackID();
            }
        }
        
        ~StackFrameImpl()
        {
        }

        lldb::StackFrameSP
        GetFrameSP ()
        {
            lldb::StackFrameSP frame_sp;
            // We have a weak pointer to our thread, which might
            // be NULL'ed out if the thread went away, so first
            // make sure our thread is still alive.
            lldb::ThreadSP thread_sp (m_thread_wp.lock());
            if (thread_sp)
            {
                // Our thread is still here, check if our frame
                // is still alive as well.
                frame_sp = m_frame_wp.lock();
                if (frame_sp)
                {
                    // Our frame is still alive, make sure that our thread
                    // still has this exact frame...
                    lldb::StackFrameSP tmp_frame_sp (thread_sp->GetStackFrameAtIndex (frame_sp->GetFrameIndex()));
                    if (tmp_frame_sp == frame_sp)
                        return frame_sp;
                }
                // The original stack frame might have gone away,
                // we need to check for the frame by stack ID
                frame_sp = thread_sp->GetFrameWithStackID (m_stack_id);
                m_frame_wp = frame_sp;
            }
            return frame_sp;
        }

        void
        SetFrameSP (const lldb::StackFrameSP &frame_sp)
        {
            if (frame_sp)
            {
                m_frame_wp = frame_sp;
                m_thread_wp = frame_sp->GetThread();
                m_stack_id = frame_sp->GetStackID();
            }
            else
            {
                m_frame_wp.reset();
                m_thread_wp.reset();
                m_stack_id.Clear();
            }
        }

    protected:
        lldb::StackFrameWP m_frame_wp;
        lldb::ThreadWP m_thread_wp;
        StackID m_stack_id;
    };
} // namespace lldb_private

using namespace lldb;
using namespace lldb_private;


SBFrame::SBFrame () :
    m_opaque_sp ()
{
}

SBFrame::SBFrame (const StackFrameSP &lldb_object_sp) :
    m_opaque_sp (new StackFrameImpl (lldb_object_sp))
{
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));

    if (log)
    {
        SBStream sstr;
        GetDescription (sstr);
        log->Printf ("SBFrame::SBFrame (sp=%p) => SBFrame(%p): %s", 
                     lldb_object_sp.get(), lldb_object_sp.get(), sstr.GetData());
                     
    }
}

SBFrame::SBFrame(const SBFrame &rhs) :
    m_opaque_sp (rhs.m_opaque_sp)
{
}

const SBFrame &
SBFrame::operator = (const SBFrame &rhs)
{
    if (this != &rhs)
        m_opaque_sp = rhs.m_opaque_sp;
    return *this;
}

SBFrame::~SBFrame()
{
}

StackFrameSP
SBFrame::GetFrameSP() const
{
    StackFrameImplSP impl_sp (m_opaque_sp);
    StackFrameSP frame_sp;
    if (impl_sp)
        frame_sp = impl_sp->GetFrameSP();
    return frame_sp;
}

void
SBFrame::SetFrameSP (const StackFrameSP &lldb_object_sp)
{
    if (lldb_object_sp)
    {
        if (m_opaque_sp)
        {
            StackFrameImplSP impl_sp (m_opaque_sp);
            if (impl_sp)
                impl_sp->SetFrameSP (lldb_object_sp);
        }
        else
        {
            m_opaque_sp = StackFrameImplSP (new StackFrameImpl(lldb_object_sp));
        }
    }
    else
    {
        m_opaque_sp.reset();
    }
}

bool
SBFrame::IsValid() const
{
    StackFrameImplSP impl_sp (m_opaque_sp);
    if (impl_sp)
        return (impl_sp->GetFrameSP().get() != NULL);
    return false;
}

SBSymbolContext
SBFrame::GetSymbolContext (uint32_t resolve_scope) const
{

    SBSymbolContext sb_sym_ctx;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_sym_ctx.SetSymbolContext(&frame->GetSymbolContext (resolve_scope));
    }

    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetSymbolContext (resolve_scope=0x%8.8x) => SBSymbolContext(%p)", 
                     frame, resolve_scope, sb_sym_ctx.get());

    return sb_sym_ctx;
}

SBModule
SBFrame::GetModule () const
{
    SBModule sb_module;
    ModuleSP module_sp;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        module_sp = frame->GetSymbolContext (eSymbolContextModule).module_sp;
        sb_module.SetSP (module_sp);
    }

    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetModule () => SBModule(%p)", 
                     frame, module_sp.get());

    return sb_module;
}

SBCompileUnit
SBFrame::GetCompileUnit () const
{
    SBCompileUnit sb_comp_unit;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_comp_unit.reset (frame->GetSymbolContext (eSymbolContextCompUnit).comp_unit);
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetModule () => SBCompileUnit(%p)", 
                     frame, sb_comp_unit.get());

    return sb_comp_unit;
}

SBFunction
SBFrame::GetFunction () const
{
    SBFunction sb_function;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_function.reset(frame->GetSymbolContext (eSymbolContextFunction).function);
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetFunction () => SBFunction(%p)", 
                     frame, sb_function.get());

    return sb_function;
}

SBSymbol
SBFrame::GetSymbol () const
{
    SBSymbol sb_symbol;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_symbol.reset(frame->GetSymbolContext (eSymbolContextSymbol).symbol);
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetSymbol () => SBSymbol(%p)", 
                     frame, sb_symbol.get());
    return sb_symbol;
}

SBBlock
SBFrame::GetBlock () const
{
    SBBlock sb_block;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_block.SetPtr (frame->GetSymbolContext (eSymbolContextBlock).block);
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetBlock () => SBBlock(%p)", 
                     frame, sb_block.GetPtr());
    return sb_block;
}

SBBlock
SBFrame::GetFrameBlock () const
{
    SBBlock sb_block;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_block.SetPtr(frame->GetFrameBlock ());
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetFrameBlock () => SBBlock(%p)", 
                     frame, sb_block.GetPtr());
    return sb_block;    
}

SBLineEntry
SBFrame::GetLineEntry () const
{
    SBLineEntry sb_line_entry;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_line_entry.SetLineEntry (frame->GetSymbolContext (eSymbolContextLineEntry).line_entry);
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetLineEntry () => SBLineEntry(%p)", 
                     frame, sb_line_entry.get());
    return sb_line_entry;
}

uint32_t
SBFrame::GetFrameID () const
{
    uint32_t frame_idx = UINT32_MAX;
    
    
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
        frame_idx = frame->GetFrameIndex ();
    
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetFrameID () => %u", 
                     frame, frame_idx);
    return frame_idx;
}

addr_t
SBFrame::GetPC () const
{
    addr_t addr = LLDB_INVALID_ADDRESS;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        addr = frame->GetFrameCodeAddress().GetOpcodeLoadAddress (target);
    }

    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetPC () => 0x%llx", frame, addr);

    return addr;
}

bool
SBFrame::SetPC (addr_t new_pc)
{
    bool ret_val = false;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        ret_val = frame->GetRegisterContext()->SetPC (new_pc);
    }

    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::SetPC (new_pc=0x%llx) => %i", 
                     frame, new_pc, ret_val);

    return ret_val;
}

addr_t
SBFrame::GetSP () const
{
    addr_t addr = LLDB_INVALID_ADDRESS;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        addr = frame->GetRegisterContext()->GetSP();
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetSP () => 0x%llx", frame, addr);

    return addr;
}


addr_t
SBFrame::GetFP () const
{
    addr_t addr = LLDB_INVALID_ADDRESS;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        addr = frame->GetRegisterContext()->GetFP();
    }

    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetFP () => 0x%llx", frame, addr);
    return addr;
}


SBAddress
SBFrame::GetPCAddress () const
{
    SBAddress sb_addr;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        sb_addr.SetAddress (&frame->GetFrameCodeAddress());
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::GetPCAddress () => SBAddress(%p)", frame, sb_addr.get());
    return sb_addr;
}

void
SBFrame::Clear()
{
    m_opaque_sp.reset();
}

lldb::SBValue
SBFrame::GetValueForVariablePath (const char *var_path)
{
    SBValue sb_value;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        lldb::DynamicValueType  use_dynamic = frame->CalculateTarget()->GetPreferDynamicValue();
        sb_value = GetValueForVariablePath (var_path, use_dynamic);
    }
    return sb_value;
}

lldb::SBValue
SBFrame::GetValueForVariablePath (const char *var_path, DynamicValueType use_dynamic)
{
    SBValue sb_value;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target && var_path && var_path[0])
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        VariableSP var_sp;
        Error error;
        ValueObjectSP value_sp (frame->GetValueForVariableExpressionPath (var_path, 
                                                                          use_dynamic,
                                                                          StackFrame::eExpressionPathOptionCheckPtrVsMember,
                                                                          var_sp,
                                                                          error));
        sb_value.SetSP(value_sp);
    }
    return sb_value;
}

SBValue
SBFrame::FindVariable (const char *name)
{
    SBValue value;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        lldb::DynamicValueType  use_dynamic = frame->CalculateTarget()->GetPreferDynamicValue();
        value = FindVariable (name, use_dynamic);
    }
    return value;
}
                                    

SBValue
SBFrame::FindVariable (const char *name, lldb::DynamicValueType use_dynamic)
{
    VariableSP var_sp;
    SBValue sb_value;
    ValueObjectSP value_sp;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target && name && name[0])
    {
        VariableList variable_list;
        Mutex::Locker api_locker (target->GetAPIMutex());
        SymbolContext sc (frame->GetSymbolContext (eSymbolContextBlock));

        if (sc.block)
        {
            const bool can_create = true;
            const bool get_parent_variables = true;
            const bool stop_if_block_is_inlined_function = true;
            
            if (sc.block->AppendVariables (can_create, 
                                           get_parent_variables,
                                           stop_if_block_is_inlined_function,
                                           &variable_list))
            {
                var_sp = variable_list.FindVariable (ConstString(name));
            }
        }

        if (var_sp)
        {
            value_sp = frame->GetValueObjectForFrameVariable(var_sp, use_dynamic);
            sb_value.SetSP(value_sp);
        }
        
    }
    
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::FindVariable (name=\"%s\") => SBValue(%p)", 
                     frame, name, value_sp.get());

    return sb_value;
}

SBValue
SBFrame::FindValue (const char *name, ValueType value_type)
{
    SBValue value;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        lldb::DynamicValueType use_dynamic = frame->CalculateTarget()->GetPreferDynamicValue();
        value = FindValue (name, value_type, use_dynamic);
    }
    return value;
}

SBValue
SBFrame::FindValue (const char *name, ValueType value_type, lldb::DynamicValueType use_dynamic)
{
    SBValue sb_value;
    ValueObjectSP value_sp;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target && name && name[0])
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
    
        switch (value_type)
        {
        case eValueTypeVariableGlobal:      // global variable
        case eValueTypeVariableStatic:      // static variable
        case eValueTypeVariableArgument:    // function argument variables
        case eValueTypeVariableLocal:       // function local variables
            {
                VariableList *variable_list = frame->GetVariableList(true);

                SymbolContext sc (frame->GetSymbolContext (eSymbolContextBlock));

                const bool can_create = true;
                const bool get_parent_variables = true;
                const bool stop_if_block_is_inlined_function = true;

                if (sc.block && sc.block->AppendVariables (can_create, 
                                                           get_parent_variables,
                                                           stop_if_block_is_inlined_function,
                                                           variable_list))
                {
                    ConstString const_name(name);
                    const uint32_t num_variables = variable_list->GetSize();
                    for (uint32_t i = 0; i < num_variables; ++i)
                    {
                        VariableSP variable_sp (variable_list->GetVariableAtIndex(i));
                        if (variable_sp && 
                            variable_sp->GetScope() == value_type &&
                            variable_sp->GetName() == const_name)
                        {
                            value_sp = frame->GetValueObjectForFrameVariable (variable_sp, use_dynamic);
                            sb_value.SetSP (value_sp);
                            break;
                        }
                    }
                }
            }
            break;

        case eValueTypeRegister:            // stack frame register value
            {
                RegisterContextSP reg_ctx (frame->GetRegisterContext());
                if (reg_ctx)
                {
                    const uint32_t num_regs = reg_ctx->GetRegisterCount();
                    for (uint32_t reg_idx = 0; reg_idx < num_regs; ++reg_idx)
                    {
                        const RegisterInfo *reg_info = reg_ctx->GetRegisterInfoAtIndex (reg_idx);
                        if (reg_info && 
                            ((reg_info->name && strcasecmp (reg_info->name, name) == 0) ||
                             (reg_info->alt_name && strcasecmp (reg_info->alt_name, name) == 0)))
                        {
                            value_sp = ValueObjectRegister::Create (frame, reg_ctx, reg_idx);
                            sb_value.SetSP (value_sp);
                            break;
                        }
                    }
                }
            }
            break;

        case eValueTypeRegisterSet:         // A collection of stack frame register values
            {
                RegisterContextSP reg_ctx (frame->GetRegisterContext());
                if (reg_ctx)
                {
                    const uint32_t num_sets = reg_ctx->GetRegisterSetCount();
                    for (uint32_t set_idx = 0; set_idx < num_sets; ++set_idx)
                    {
                        const RegisterSet *reg_set = reg_ctx->GetRegisterSet (set_idx);
                        if (reg_set && 
                            ((reg_set->name && strcasecmp (reg_set->name, name) == 0) ||
                             (reg_set->short_name && strcasecmp (reg_set->short_name, name) == 0)))
                        {
                            value_sp = ValueObjectRegisterSet::Create (frame, reg_ctx, set_idx);
                            sb_value.SetSP (value_sp);
                            break;
                        }
                    }
                }
            }
            break;

        case eValueTypeConstResult:         // constant result variables
            {
                ConstString const_name(name);
                ClangExpressionVariableSP expr_var_sp (target->GetPersistentVariables().GetVariable (const_name));
                if (expr_var_sp)
                {
                    value_sp = expr_var_sp->GetValueObject();
                    sb_value.SetSP (value_sp);
                }
            }
            break;

        default:
            break;
        }
    }
    
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    if (log)
        log->Printf ("SBFrame(%p)::FindVariableInScope (name=\"%s\", value_type=%i) => SBValue(%p)", 
                     frame, name, value_type, value_sp.get());

    
    return sb_value;
}

bool
SBFrame::IsEqual (const SBFrame &that) const
{
    lldb::StackFrameSP this_sp = GetFrameSP();
    lldb::StackFrameSP that_sp = that.GetFrameSP();
    return (this_sp && that_sp && this_sp->GetStackID() == that_sp->GetStackID());
}

bool
SBFrame::operator == (const SBFrame &rhs) const
{
    return IsEqual(rhs);
}

bool
SBFrame::operator != (const SBFrame &rhs) const
{
    return !IsEqual(rhs);
}

SBThread
SBFrame::GetThread () const
{
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));

    ExecutionContext exe_ctx(GetFrameSP());
    ThreadSP thread_sp (exe_ctx.GetThreadSP());
    SBThread sb_thread (thread_sp);

    if (log)
    {
        SBStream sstr;
        sb_thread.GetDescription (sstr);
        log->Printf ("SBFrame(%p)::GetThread () => SBThread(%p): %s", 
                     exe_ctx.GetFramePtr(), 
                     thread_sp.get(), 
                     sstr.GetData());
    }

    return sb_thread;
}

const char *
SBFrame::Disassemble () const
{
    const char *disassembly = NULL;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        disassembly = frame->Disassemble();
    }
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));

    if (log)
        log->Printf ("SBFrame(%p)::Disassemble () => %s", frame, disassembly);

    return disassembly;
}


SBValueList
SBFrame::GetVariables (bool arguments,
                       bool locals,
                       bool statics,
                       bool in_scope_only)
{
    SBValueList value_list;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        lldb::DynamicValueType use_dynamic = frame->CalculateTarget()->GetPreferDynamicValue();
        value_list = GetVariables (arguments, locals, statics, in_scope_only, use_dynamic);
    }
    return value_list;
}

SBValueList
SBFrame::GetVariables (bool arguments,
                       bool locals,
                       bool statics,
                       bool in_scope_only,
                       lldb::DynamicValueType  use_dynamic)
{
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));

    SBValueList value_list;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();

    if (log)
        log->Printf ("SBFrame(%p)::GetVariables (arguments=%i, locals=%i, statics=%i, in_scope_only=%i)", 
                     frame, 
                     arguments,
                     locals,
                     statics,
                     in_scope_only);
    
    if (frame && target)
    {

        size_t i;
        VariableList *variable_list = NULL;
        // Scope for locker
        {
            Mutex::Locker api_locker (target->GetAPIMutex());
            variable_list = frame->GetVariableList(true);
        }
        if (variable_list)
        {
            const size_t num_variables = variable_list->GetSize();
            if (num_variables)
            {
                for (i = 0; i < num_variables; ++i)
                {
                    VariableSP variable_sp (variable_list->GetVariableAtIndex(i));
                    if (variable_sp)
                    {
                        bool add_variable = false;
                        switch (variable_sp->GetScope())
                        {
                        case eValueTypeVariableGlobal:
                        case eValueTypeVariableStatic:
                            add_variable = statics;
                            break;

                        case eValueTypeVariableArgument:
                            add_variable = arguments;
                            break;

                        case eValueTypeVariableLocal:
                            add_variable = locals;
                            break;

                        default:
                            break;
                        }
                        if (add_variable)
                        {
                            if (in_scope_only && !variable_sp->IsInScope(frame))
                                continue;

                            value_list.Append(frame->GetValueObjectForFrameVariable (variable_sp, use_dynamic));
                        }
                    }
                }
            }
        }        
    }

    if (log)
    {
        log->Printf ("SBFrame(%p)::GetVariables (...) => SBValueList(%p)", frame,
                     value_list.get());
    }

    return value_list;
}

SBValueList
SBFrame::GetRegisters ()
{
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));

    SBValueList value_list;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        RegisterContextSP reg_ctx (frame->GetRegisterContext());
        if (reg_ctx)
        {
            const uint32_t num_sets = reg_ctx->GetRegisterSetCount();
            for (uint32_t set_idx = 0; set_idx < num_sets; ++set_idx)
            {
                value_list.Append(ValueObjectRegisterSet::Create (frame, reg_ctx, set_idx));
            }
        }
    }

    if (log)
        log->Printf ("SBFrame(%p)::Registers () => SBValueList(%p)", frame, value_list.get());

    return value_list;
}

bool
SBFrame::GetDescription (SBStream &description)
{
    Stream &strm = description.ref();

    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        frame->DumpUsingSettingsFormat (&strm);
    }
    else
        strm.PutCString ("No value");

    return true;
}

SBValue
SBFrame::EvaluateExpression (const char *expr)
{
    SBValue result;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        lldb::DynamicValueType use_dynamic = frame->CalculateTarget()->GetPreferDynamicValue();
        result = EvaluateExpression (expr, use_dynamic);
    }
    return result;
}

SBValue
SBFrame::EvaluateExpression (const char *expr, lldb::DynamicValueType fetch_dynamic_value)
{
    LogSP log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_API));
    
    LogSP expr_log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    ExecutionResults exe_results;
    SBValue expr_result;
    ValueObjectSP expr_value_sp;

    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (log)
        log->Printf ("SBFrame(%p)::EvaluateExpression (expr=\"%s\")...", frame, expr);

    if (frame && target)
    {
        Mutex::Locker api_locker (target->GetAPIMutex());
        
        
        StreamString frame_description;
        frame->DumpUsingSettingsFormat (&frame_description);

        Host::SetCrashDescriptionWithFormat ("SBFrame::EvaluateExpression (expr = \"%s\", fetch_dynamic_value = %u) %s",
                                             expr, fetch_dynamic_value, frame_description.GetString().c_str());

        const bool coerce_to_id = false;
        const bool unwind_on_error = true;
        const bool keep_in_memory = false;

        exe_results = target->EvaluateExpression (expr, 
                                                  frame,
                                                  eExecutionPolicyOnlyWhenNeeded,
                                                  coerce_to_id,
                                                  unwind_on_error, 
                                                  keep_in_memory, 
                                                  fetch_dynamic_value, 
                                                  expr_value_sp);
        expr_result.SetSP(expr_value_sp);
        Host::SetCrashDescription (NULL);
    }

#ifndef LLDB_DISABLE_PYTHON
    if (expr_log)
        expr_log->Printf("** [SBFrame::EvaluateExpression] Expression result is %s, summary %s **", 
                         expr_result.GetValue(), 
                         expr_result.GetSummary());
    
    if (log)
        log->Printf ("SBFrame(%p)::EvaluateExpression (expr=\"%s\") => SBValue(%p) (execution result=%d)", 
                     frame, 
                     expr, 
                     expr_value_sp.get(),
                     exe_results);
#endif

    return expr_result;
}

bool
SBFrame::IsInlined()
{
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        Block *block = frame->GetSymbolContext(eSymbolContextBlock).block;
        if (block)
            return block->GetContainingInlinedBlock () != NULL;
    }
    return false;
}

const char *
SBFrame::GetFunctionName()
{
    const char *name = NULL;
    ExecutionContext exe_ctx(GetFrameSP());
    StackFrame *frame = exe_ctx.GetFramePtr();
    Target *target = exe_ctx.GetTargetPtr();
    if (frame && target)
    {
        SymbolContext sc (frame->GetSymbolContext(eSymbolContextFunction | eSymbolContextBlock | eSymbolContextSymbol));
        if (sc.block)
        {
            Block *inlined_block = sc.block->GetContainingInlinedBlock ();
            if (inlined_block)
            {
                const InlineFunctionInfo* inlined_info = inlined_block->GetInlinedFunctionInfo();
                name = inlined_info->GetName().AsCString();
            }
        }
        
        if (name == NULL)
        {
            if (sc.function)
                name = sc.function->GetName().GetCString();
        }

        if (name == NULL)
        {
            if (sc.symbol)
                name = sc.symbol->GetName().GetCString();
        }
    }
    return name;
}

